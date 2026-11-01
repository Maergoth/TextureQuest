"""Verified DDS downloads and immutable objects. Never writes a game archive."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import uuid
import zipfile
from urllib.request import urlopen
from texture_swapper import core
from texturequest_latest import _read, version_number
from texturequest_release import check_pause
from texturequest_direct import _download
import texturequest_graphics_catalog as catalog
from texturequest_shared_textures import resolve

REPOSITORY = 'https://github.com/Maergoth/TextureQuest-Textures'
API = 'https://api.github.com/repos/Maergoth/TextureQuest-Textures/releases/latest'
MAX_INDEX = 128 * 1024**2


def need(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def pin_file(path, pin):
    with catalog.held(path) as (stream, identity):
        need(identity[2] == pin['bytes'] and catalog.sha_file(stream) == pin['sha256'], 'File checksum: ' + str(path))


def validate_pin(pin, maximum, pattern):
    need(isinstance(pin, dict) and isinstance(pin.get('name'), str) and
         re.fullmatch(pattern, pin['name']) is not None and type(pin.get('bytes')) is int and
         0 < pin['bytes'] <= maximum and re.fullmatch('[a-f0-9]{64}', str(pin.get('sha256'))) is not None,
         'Invalid DDS download identity')
    need(re.fullmatch(re.escape(REPOSITORY) + r'/releases/download/v\d+\.\d+\.\d+/' + re.escape(pin['name']),
                      str(pin.get('url'))) is not None, 'Unexpected DDS download URL')


def validate_source(source):
    need(source.get('schema') == 'texturequest-dds-source-v1' and source.get('distribution_version') == 1,
         'Unsupported DDS source')
    version_number(source['texture_version'])
    validate_pin(source['index'], MAX_INDEX, r'texture-index\.json')
    chunks = source['chunks']
    need(isinstance(chunks, list) and 0 < len(chunks) <= 4096, 'Invalid DDS chunk inventory')
    for chunk in chunks:
        validate_pin(chunk, 2 * 1024**3, r'dds-\d{3}-[a-f0-9]{12}\.zip')
        need(type(chunk['objects']) is int and 0 < chunk['objects'] <= 200000, 'Invalid DDS chunk count')
    need(len({r['name'] for r in chunks}) == len(chunks), 'Duplicate DDS chunk')
    need(sum(r['bytes'] for r in chunks) == source['compressed_download_bytes'], 'DDS download accounting')
    return source


def fetch_source(opener=urlopen, pause=None, minimum='1.2.4'):
    metadata = core._json(_read(API, 8 * 1024**2, opener, pause))
    tag = metadata.get('tag_name', '')
    need(tag.startswith('v') and version_number(tag[1:]) >= version_number(minimum) and
         metadata.get('draft') is False and metadata.get('prerelease') is False and metadata.get('published_at') and
         metadata.get('html_url') == REPOSITORY + '/releases/tag/' + tag, 'Unexpected DDS release')
    assets = [r for r in metadata['assets'] if r.get('name') == 'texture-source.json']
    need(len(assets) == 1, 'DDS release has no unique texture-source.json')
    asset = assets[0]
    pin = dict(name=asset['name'], bytes=asset['size'], sha256=str(asset['digest']).removeprefix('sha256:'),
               url=asset['browser_download_url'])
    validate_pin(pin, 4 * 1024**2, r'texture-source\.json')
    need(pin['url'] == REPOSITORY + '/releases/download/' + tag + '/texture-source.json' and
         asset.get('state') == 'uploaded', 'Unexpected DDS source asset')
    raw = _read(pin['url'], pin['bytes'], opener, pause)
    need(len(raw) == pin['bytes'] and sha(raw) == pin['sha256'], 'DDS source checksum')
    source = validate_source(core._json(raw))
    need(source['texture_version'] == tag[1:], 'DDS source version differs from release')
    return source


def validate_index(index, source):
    need(index.get('schema') == 'texturequest-dds-index-v1' and index.get('fingerprint_schema') == 'TQABUP01' and
         index.get('texture_version') == source['texture_version'], 'Unsupported DDS index')
    objects = index['objects']
    rows = index['named_textures']
    need(0 < len(objects) == source['unique_dds_count'] <= 100000 and
         0 < len(rows) == source['named_texture_count'] <= 200000, 'DDS index cardinality')
    pins = {o['sha256']: o for o in objects}
    need(len(pins) == len(objects), 'Duplicate DDS object')
    chunks = {c['name']: c for c in source['chunks']}
    for obj in objects:
        need(re.fullmatch('[a-f0-9]{64}', str(obj['sha256'])) is not None and
             obj['entry'] == 'dds/' + obj['sha256'] + '.dds' and obj['chunk'] in chunks and
             type(obj['bytes']) is int and 128 <= obj['bytes'] <= catalog.MAX_DDS + 4096,
             'Invalid DDS object')
    need(len({r['id'] for r in rows}) == len(rows), 'Duplicate named DDS identity')
    for row in rows:
        core._name(row['owner'])
        core._member(row['member'])
        need(row['id'] == sha((row['owner'].lower() + '\0' + row['member']).encode('utf-8')) and
             row['object_sha256'] in pins, 'Named DDS identity differs')
        for key in ('source', 'target'):
            value = row[key]
            catalog.mip_levels(tuple(value['layout']))
            need(re.fullmatch('[a-f0-9]{64}', value['fingerprint']) is not None, 'Invalid DDS fingerprint')
        need(re.fullmatch('[a-f0-9]{64}', row['source']['dds_sha256']) is not None, 'Invalid source DDS identity')
    need(set(pins) == {r['object_sha256'] for r in rows}, 'Unreferenced DDS objects')
    need(sum(o['bytes'] for o in objects) == source['uncompressed_dds_bytes'], 'DDS object byte accounting')
    for name, chunk in chunks.items():
        need(sum(o['chunk'] == name for o in objects) == chunk['objects'], 'DDS chunk object accounting')
    return index


def verify_dds(raw, obj, targets):
    need(len(raw) == obj['bytes'] and sha(raw) == obj['sha256'], 'DDS object checksum')
    layout, pixels, tail = catalog.decode_dds(raw)
    for target in targets:
        need(list(layout) == target['layout'] and len(pixels) == target['mip_bytes'] and
             sha(pixels) == target['mip_sha256'] and catalog.fingerprint(layout, pixels) == target['fingerprint'] and
             len(tail) == target['tail_bytes'] and sha(tail) == target['tail_sha256'], 'DDS layout/mip/tail mismatch')


class DDSStore:
    def __init__(self, state_dir):
        self.state_dir = core._plain(state_dir)
        self.base = self.state_dir / 'dds'
        self.objects = self.base / 'objects'

    def summary(self):
        """Read the small cache receipt for display, without checking its files.

        This is not readiness or installation authority. saved() retains its
        inventory checks; normal installation still verifies every object.
        """
        try:
            pointer = core._plain(self.state_dir / 'dds-prepared.json', self.state_dir)
            need(0 < pointer.stat().st_size <= 64 * 1024, 'DDS summary size differs')
            value = core._load(pointer)
            need(value.get('schema') == 'texturequest-loose-dds-prepared-v1', 'Unsupported DDS preparation')
            version_number(value['texture_version'])
            need(type(value['object_count']) is int and 0 < value['object_count'] <= 100000 and
                 type(value['object_bytes']) is int and 0 < value['object_bytes'] <= value['object_count'] * (catalog.MAX_DDS + 4096) and
                 type(value['named_textures']) is int and 0 < value['named_textures'] <= 200000,
                 'Invalid DDS summary counts')
            need(isinstance(value['game'], str) and isinstance(value['object_root'], str), 'Invalid DDS summary paths')
            for key in ('source_sha256', 'index_sha256', 'choices_sha256'):
                need(re.fullmatch('[a-f0-9]{64}', str(value[key])) is not None, 'Invalid DDS summary identity')
            for key in ('objects', 'map'):
                pin = value[key]
                need(isinstance(pin, dict) and isinstance(pin.get('path'), str) and
                     type(pin.get('bytes')) is int and pin['bytes'] > 0 and
                     re.fullmatch('[a-f0-9]{64}', str(pin.get('sha256'))) is not None,
                     'Invalid DDS summary file identity')
            coverage = value.get('coverage', {})
            need(isinstance(coverage, dict) and type(coverage.get('pending_groups', 0)) is int and
                 0 <= coverage.get('pending_groups', 0) <= 200000, 'Invalid DDS summary coverage')
            return value
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            return None

    def saved(self):
        try:
            value = core._load(self.state_dir / 'dds-prepared.json')
            self.verify(value, full=False)
            return value
        except (OSError, ValueError, KeyError, TypeError):
            return None

    def _metadata(self, value):
        raw = json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
        path = self.base / 'metadata' / (sha(raw) + '.json')
        if not path.exists():
            _write_new(path, raw)
        return dict(path=str(path), bytes=len(raw), sha256=sha(raw))

    def object_rows(self, prepared):
        pin = prepared['objects']
        path = core._plain(pin['path'], self.base / 'metadata')
        need(type(pin['bytes']) is int and 0 < pin['bytes'] <= MAX_INDEX, 'DDS roster size differs')
        pin_file(path, pin)
        rows = core._json(path.read_bytes())
        need(isinstance(rows, list) and len(rows) == prepared['object_count'] <= 100000, 'DDS roster count differs')
        need(sum(r['bytes'] for r in rows) == prepared['object_bytes'], 'DDS roster byte accounting')
        return rows

    def verify(self, prepared, full=True):
        need(prepared.get('schema') == 'texturequest-loose-dds-prepared-v1', 'Unsupported DDS preparation')
        need(core._plain(prepared['object_root']) == self.objects, 'DDS root changed')
        for obj in self.object_rows(prepared):
            need(obj['entry'] == 'dds/' + obj['sha256'] + '.dds' and
                 re.fullmatch('[a-f0-9]{64}', obj['sha256']) is not None, 'DDS prepared path changed')
            path = core._plain(self.objects / obj['entry'], self.objects)
            need(path.is_file() and path.stat().st_size == obj['bytes'], 'Saved DDS missing: ' + obj['sha256'])
            if full:
                pin_file(path, obj)
        map_path = core._plain(prepared['map']['path'], self.base)
        pin_file(map_path, prepared['map'])
        need(map_path.read_bytes()[:8] == b'TQMAP002', 'Prepared DDS map format differs')
        return prepared

    def prepare(self, source, game, choices, *, opener=urlopen, pause=None, progress=lambda _: None,
                overall=lambda *_: None, in_use=lambda: False):
        from texturequest_loose_map import build
        source = validate_source(source)
        game = core._plain(core.validate_game(game).parent)
        self.base.mkdir(parents=True, exist_ok=True)
        with core._lock(self.state_dir / 'operation.lock'), core._lock(self.state_dir / 'sideload/helper-owner.lock'):
            need(not in_use(), 'Close EQ2 and remove the old injected hook before preparing textures.')
            pointer = self.state_dir / 'dds-prepared.json'
            if pointer.exists():
                previous = core._load(pointer)
                need(previous.get('schema') == 'texturequest-loose-dds-prepared-v1', 'Unknown DDS receipt is preserved')
                need(version_number(source['texture_version']) >= version_number(previous['texture_version']), 'DDS downgrade refused')
                if source['texture_version'] == previous['texture_version']:
                    need(core._digest(source) == previous['source_sha256'], 'Published DDS bytes changed without a texture version change')
            metadata = self.base / 'metadata'
            metadata.mkdir(exist_ok=True)
            index_path = core._plain(metadata / (source['index']['sha256'] + '.json'), metadata)
            if not index_path.exists():
                raw = _read(source['index']['url'], source['index']['bytes'], opener, pause)
                need(len(raw) == source['index']['bytes'] and sha(raw) == source['index']['sha256'], 'DDS index checksum')
                _write_new(index_path, raw)
            pin_file(index_path, source['index'])
            index = validate_index(core._json(index_path.read_bytes()), source)
            selected, aliases = resolve(index, choices)
            wanted = {r['object_sha256'] for r in selected}
            objects = [o for o in index['objects'] if o['sha256'] in wanted]
            targets = {}
            for row in selected:
                targets.setdefault(row['object_sha256'], []).append(row['target'])
            chunks = {c['name']: c for c in source['chunks']}
            all_objects = {o['entry']: o for o in index['objects']}
            self.objects.mkdir(exist_ok=True)
            (self.objects / 'dds').mkdir(exist_ok=True)
            downloads = self.base / 'downloads'
            downloads.mkdir(exist_ok=True)
            total = sum(o['bytes'] for o in objects)
            done, complete = 0, 0
            for name, chunk in chunks.items():
                check_pause(pause)
                required = [o for o in objects if o['chunk'] == name]
                missing = []
                for obj in required:
                    path = core._plain(self.objects / obj['entry'], self.objects)
                    if path.exists():
                        pin_file(path, obj)
                        done += obj['bytes']; complete += 1
                    else:
                        missing.append(obj)
                overall('Verifying saved DDS files', done, total, complete, len(objects))
                if not missing:
                    continue
                need(not in_use(), 'Close EQ2 before completing the texture update.')
                need(shutil.disk_usage(self.base).free >= chunk['bytes'] + sum(o['bytes'] for o in missing) + 64 * core.CHUNK,
                     'Not enough space for this DDS download and extracted files')
                row = dict(chunk, entries=chunk['objects'])
                row.pop('objects')
                # Reuse the bounded, journaled range downloader; it has no game writes.
                path = _download(row, downloads, notify=lambda m, *_: progress(m), pause=pause, opener=opener)
                expected = {o['entry']: o for o in index['objects'] if o['chunk'] == name}
                with catalog.held(path), zipfile.ZipFile(path) as archive:
                    entries = archive.infolist()
                    need(len(entries) == len(expected) and {i.filename for i in entries} == set(expected), 'DDS ZIP membership differs')
                    for info in entries:
                        obj = all_objects[info.filename]
                        need(info.file_size == obj['bytes'] and not info.flag_bits & 1 and
                             stat.S_IFMT(info.external_attr >> 16) == stat.S_IFREG, 'DDS ZIP entry type/size differs')
                    for obj in missing:
                        check_pause(pause)
                        raw = archive.read(obj['entry'])
                        verify_dds(raw, obj, targets[obj['sha256']])
                        _write_new(core._plain(self.objects / obj['entry'], self.objects), raw)
                        done += obj['bytes']; complete += 1
                        overall('Saved DDS ' + str(complete) + '/' + str(len(objects)), done, total, complete, len(objects))
                # Only this verified owned ZIP is removed; immutable DDS survive interrupts.
                path.unlink()
            check_pause(pause)
            need(not in_use(), 'Close EQ2 before activating the new texture map.')
            raw, coverage = build(game, index, selected, aliases, self.objects, core._digest(source), progress, pause)
            bundle = self.base / 'maps'
            bundle.mkdir(exist_ok=True)
            map_path = bundle / (sha(raw) + '.bin')
            if not map_path.exists():
                _write_new(map_path, raw)
            value = dict(schema='texturequest-loose-dds-prepared-v1', texture_version=source['texture_version'],
                         source_sha256=core._digest(source), index_sha256=source['index']['sha256'],
                         choices_sha256=core._digest(choices), game=str(game), object_root=str(self.objects),
                         objects=self._metadata(objects), object_count=len(objects),
                         object_bytes=sum(o['bytes'] for o in objects), named_textures=source['named_texture_count'],
                         map=dict(path=str(map_path), bytes=len(raw), sha256=sha(raw)),
                         coverage=dict(mapped_groups=coverage['mapped_groups'], selected_groups=coverage['selected_groups'],
                                       pending_groups=coverage['pending_groups'], report=self._metadata(coverage)))
            self.verify(value, full=False)
            # All target paths are immutable. A failed/partial update leaves the old
            # map and its referenced DDS intact; only the final pointer is replaced.
            if pointer.exists():
                history = self.base / 'receipts'
                history.mkdir(exist_ok=True)
                old = pointer.read_bytes()
                saved = history / (sha(old) + '.json')
                if not saved.exists():
                    _write_new(saved, old)
            need(len(json.dumps(value, indent=2).encode('utf-8')) < core.MAX_MANIFEST_BYTES - 4096, 'DDS receipt size limit')
            core._save(pointer, value)
            return value


def _write_new(path, raw):
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with temporary.open('xb') as stream:
            stream.write(raw); stream.flush(); os.fsync(stream.fileno())
        if path.exists():
            need(path.read_bytes() == raw, 'Immutable DDS destination changed')
        else:
            os.rename(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
