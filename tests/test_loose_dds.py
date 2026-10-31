"""Loose DDS pipeline: synthetic D-only files, real source/hash/map joins, no game/GPU."""
import copy
import hashlib
import io
import json
from pathlib import Path
import random
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from texture_swapper import core
import texturequest_graphics_catalog as catalog
import texturequest_dds_source as dds
import texturequest_shared_textures as shared
from texturequest_loose_map import FILE
import texturequest_texture_map as wire
from test_texture_map import dds as image, vpk
from test_direct import Response


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def representation(raw, source=False):
    layout, pixels, tail = catalog.decode_dds(raw)
    result = dict(layout=list(layout), fingerprint=catalog.fingerprint(layout, pixels))
    if source:
        result['dds_sha256'] = digest(raw)
    else:
        result.update(mip_bytes=len(pixels), mip_sha256=digest(pixels), tail_bytes=len(tail), tail_sha256=digest(tail))
    return result


def fixture(root, values=(3, 3, 9)):
    game = root / 'Game'
    (game / 'paks').mkdir(parents=True)
    (game / 'EverQuest2.exe').write_bytes(b'not an executable')
    stock = image(4, 1, 1)
    rows, blobs = [], {}
    for n, value in enumerate(values):
        raw = image(8, 2, value) + b'opaque-tail'
        key = digest(raw)
        blobs[key] = raw
        member = f'test/{n}.dds'
        rows.append(dict(id=digest(('test.vpk\0' + member).encode()), owner='test.vpk', member=member,
                         object_sha256=key, source=representation(stock, True), target=representation(raw), category={}))
    (game / 'paks/test.vpk').write_bytes(vpk({r['member']: stock for r in rows}))
    packed = io.BytesIO()
    with zipfile.ZipFile(packed, 'w', zipfile.ZIP_DEFLATED) as archive:
        for key, raw in blobs.items():
            info = zipfile.ZipInfo('dds/' + key + '.dds')
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, raw)
    chunk = packed.getvalue()
    name = 'dds-001-' + digest(chunk)[:12] + '.zip'
    objects = [dict(sha256=k, bytes=len(v), chunk=name, entry='dds/' + k + '.dds') for k, v in sorted(blobs.items())]
    index = dict(schema='texturequest-dds-index-v1', fingerprint_schema='TQABUP01', texture_version='1.2.4',
                 named_textures=rows, objects=objects)
    raw_index = json.dumps(index).encode()
    prefix = dds.REPOSITORY + '/releases/download/v1.2.4/'
    source = dict(schema='texturequest-dds-source-v1', distribution_version=1, texture_version='1.2.4',
                  named_texture_count=len(rows), unique_dds_count=len(objects),
                  compressed_download_bytes=len(chunk), uncompressed_dds_bytes=sum(o['bytes'] for o in objects),
                  index=dict(name='texture-index.json', bytes=len(raw_index), sha256=digest(raw_index), url=prefix+'texture-index.json'),
                  chunks=[dict(name=name, bytes=len(chunk), sha256=digest(chunk), objects=len(objects), url=prefix+name)])
    def opener(request, **kwargs):
        url = request.full_url
        raw = raw_index if url.endswith('texture-index.json') else chunk
        start = int(request.headers.get('Range', 'bytes=0-').split('=')[1].split('-')[0])
        return Response(raw, start=start, status=206 if start else 200)
    return game, source, index, blobs, opener


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game, self.source, self.index, self.blobs, self.opener = fixture(self.root)


class SharedTests(Fixture):
    def test_majority_is_shared_without_rewriting_variants(self):
        before = copy.deepcopy(self.index)
        choices = shared.propose(self.index)
        selected, report = shared.resolve(self.index, choices)
        self.assertEqual(len(selected), 1)
        self.assertEqual(selected[0]['object_sha256'], self.index['named_textures'][0]['object_sha256'])
        self.assertEqual(len(report['aliases']), 3)
        self.assertFalse(report['pending'])
        self.assertEqual(self.index, before)
        self.assertEqual(len(self.index['objects']), 2)

    def test_later_correction_beats_majority(self):
        corrected = self.index['named_textures'][-1]
        choices = shared.propose(self.index, {corrected['id']: 124})
        selected, report = shared.resolve(self.index, choices)
        self.assertEqual(selected[0]['object_sha256'], corrected['object_sha256'])
        self.assertTrue(all(r['selected'] == corrected['object_sha256'] for r in report['aliases']))

    def test_later_correction_beats_older_correction(self):
        rows = self.index['named_textures']
        choices = shared.propose(self.index, {rows[0]['id']: 121, rows[-1]['id']: 124})
        self.assertEqual(shared.resolve(self.index, choices)[0][0]['object_sha256'], rows[-1]['object_sha256'])

    def test_new_groups_are_chosen_automatically(self):
        empty = dict(schema='texturequest-shared-texture-choices-v1', choices=[], protected_ids=[])
        self.assertEqual(len(shared.resolve(self.index, empty)[0]), 1)

    def test_order_does_not_change_choice(self):
        choices = shared.propose(self.index)
        shuffled = copy.deepcopy(self.index)
        shuffled['named_textures'].reverse()
        self.assertEqual(shared.propose(shuffled), choices)

    def test_later_correction_replaces_stale_ordinary_choice_automatically(self):
        choices = shared.propose(self.index)
        corrected = self.index['named_textures'][-1]
        choices['protected_ids'] = [corrected['id']]
        choices['correction_priority'] = {corrected['id']: 125}
        selected, report = shared.resolve(self.index, choices)
        self.assertEqual(selected[0]['object_sha256'], corrected['object_sha256'])
        self.assertFalse(report['pending'])

    def test_published_correction_revision_needs_no_app_policy_update(self):
        choices = shared.propose(self.index)
        corrected = self.index['named_textures'][-1]
        corrected['correction_revision'] = 125
        selected, report = shared.resolve(self.index, choices)
        self.assertEqual(selected[0]['object_sha256'], corrected['object_sha256'])
        self.assertFalse(report['pending'])


class RawProfileTests(unittest.TestCase):
    def raw(self, pitch=0, masks=(0,0,0,0)):
        words = [0]*31
        words[0],words[2],words[3],words[4],words[6],words[18] = 124,4,4,pitch,1,32
        words[19],words[21] = 0x40,8
        words[22:26] = masks
        return b'DDS '+struct.pack('<31I',*words)+bytes(range(16))+b'tail'

    def test_exact_raw8_profile_preserves_tail(self):
        layout,pixels,tail = catalog.decode_dds(self.raw())
        self.assertEqual(layout,(4,4,1,6,1))
        self.assertEqual(pixels,bytes(range(16)))
        self.assertEqual(tail,b'tail')

    def test_nearby_raw8_profiles_are_not_promoted(self):
        for raw in [self.raw(pitch=4),self.raw(masks=(255,0,0,0))]:
            with self.assertRaisesRegex(ValueError,'explicit graphics profile'):
                catalog.decode_dds(raw)


class DownloadTests(Fixture):
    def prepare(self, **kwargs):
        store = dds.DDSStore(self.root / 'data')
        result = store.prepare(self.source, self.game, shared.propose(self.index), opener=self.opener, **kwargs)
        return store, result

    def test_full_pipeline_downloads_only_dds_and_preserves_game(self):
        before = {p.relative_to(self.game).as_posix(): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        store, prepared = self.prepare()
        self.assertEqual(prepared['coverage']['mapped_groups'], 1)
        self.assertEqual(prepared['coverage']['pending_groups'],0)
        self.assertEqual(prepared['object_count'], 1)
        self.assertEqual(Path(prepared['map']['path']).read_bytes()[:8], b'TQMAP002')
        store.verify(prepared)
        self.assertEqual(store.saved(), prepared | {'record_sha256': core._load(store.state_dir/'dds-prepared.json')['record_sha256']})
        after = {p.relative_to(self.game).as_posix(): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        self.assertEqual(before, after)
        self.assertFalse(list(store.base.rglob('*.vpk')))
        self.assertFalse(list((store.base / 'downloads').glob('*.zip')))
        self.assertEqual(len(list(store.objects.rglob('*.dds'))), 1)
        for obj in store.object_rows(prepared):
            self.assertEqual((store.objects / obj['entry']).read_bytes(), self.blobs[obj['sha256']])

    def test_index_failure_keeps_previous_preparation(self):
        store, previous = self.prepare()
        receipt = (store.state_dir / 'dds-prepared.json').read_bytes()
        bad = copy.deepcopy(self.source)
        bad['texture_version'] = '1.2.5'
        bad['index']['sha256'] = 'f' * 64
        with self.assertRaisesRegex(ValueError, 'index checksum'):
            store.prepare(bad, self.game, shared.propose(self.index), opener=self.opener)
        self.assertEqual((store.state_dir/'dds-prepared.json').read_bytes(), receipt)
        store.verify(previous)

    def test_corrupt_object_is_preserved_and_refused(self):
        store, previous = self.prepare()
        path = store.objects / store.object_rows(previous)[0]['entry']
        original = path.read_bytes()
        path.write_bytes(original[:-1] + b'X')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.prepare()
        self.assertEqual(path.read_bytes(), original[:-1] + b'X')

    def test_game_open_refuses_before_download(self):
        with self.assertRaisesRegex(ValueError, 'Close EQ2'):
            self.prepare(in_use=lambda: True)
        self.assertFalse((self.root/'data/dds-prepared.json').exists())

    def test_same_version_changed_source_refused(self):
        store, previous = self.prepare()
        changed = copy.deepcopy(self.source)
        changed['index']['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'without a texture version'):
            store.prepare(changed, self.game, shared.propose(self.index), opener=self.opener)
        store.verify(previous)

    def test_interrupted_download_resumes_verified_prefix(self):
        original = self.opener
        class Interrupted(Response):
            def __init__(self, data):
                super().__init__(data, status=200)
                self.calls = 0
            def read(self, size=-1):
                self.calls += 1
                return b'' if self.calls > 1 else super().read(min(size, 31))
        def interrupt(request, **kwargs):
            response = original(request, **kwargs)
            return response if request.full_url.endswith('texture-index.json') else Interrupted(response.getvalue())
        self.opener = interrupt
        with self.assertRaisesRegex(OSError, 'interrupted'):
            self.prepare()
        self.assertFalse((self.root/'data/dds-prepared.json').exists())
        partials = list((self.root/'data/dds/downloads').glob('*.pending'))
        self.assertEqual(len(partials), 1)
        self.assertEqual(partials[0].stat().st_size, 31)
        requests = []
        def resume(request, **kwargs):
            requests.append(dict(request.headers))
            return original(request, **kwargs)
        self.opener = resume
        store, ready = self.prepare()
        store.verify(ready)
        self.assertTrue(any(r.get('Range') == 'bytes=31-' for r in requests))

    def test_foreign_zip_member_is_refused(self):
        packed = io.BytesIO()
        with zipfile.ZipFile(packed, 'w') as archive:
            archive.writestr('../outside.dds', b'not DDS')
        blob = packed.getvalue()
        chunk = self.source['chunks'][0]
        chunk.update(bytes=len(blob), sha256=digest(blob))
        self.source['compressed_download_bytes'] = len(blob)
        original = self.opener
        self.opener = lambda request, **kw: original(request, **kw) if request.full_url.endswith('texture-index.json') else Response(blob, status=200)
        with self.assertRaisesRegex(ValueError, 'ZIP membership'):
            self.prepare()
        self.assertFalse((self.root/'outside.dds').exists())

    def test_changed_stock_is_not_rebound(self):
        (self.game/'paks/test.vpk').write_bytes(vpk({r['member']: image(4,1,33) for r in self.index['named_textures']}))
        with self.assertRaisesRegex(ValueError, 'No current stock'):
            self.prepare()
        self.assertFalse((self.root/'data/dds-prepared.json').exists())

    def test_external_chunk_url_refused(self):
        self.source['chunks'][0]['url'] = 'https://example.com/a.zip'
        with self.assertRaisesRegex(ValueError, 'URL'):
            self.prepare()

    def test_native_file_table_binds_exact_dds(self):
        store, prepared = self.prepare()
        raw = Path(prepared['map']['path']).read_bytes()
        magic, major, minor, patch, digest_bytes, files, stocks, rows, strings = wire.HEADER.unpack_from(raw)
        self.assertEqual((magic,files,stocks,rows), (b'TQMAP002',1,1,1))
        size, offset, expected = FILE.unpack_from(raw, wire.HEADER.size)
        obj = store.object_rows(prepared)[0]
        self.assertEqual((size,expected.hex()), (obj['bytes'],obj['sha256']))
        text = raw[-strings:]
        self.assertEqual(text[offset:text.index(b'\0',offset)].decode(), obj['entry'])


if __name__ == '__main__':
    unittest.main()
