"""One-click migration of owned installs; never restore or delete game PAKs."""
from contextlib import ExitStack
from pathlib import Path
import hashlib
import re
import uuid
import zipfile
import time

from texture_swapper import core
from texturequest_processes import ensure_closed
from texturequest_direct import DirectSwapper, FileCustody, _obsolete_archives, LEGACY_PACKAGES
from texturequest_custody import DirectoryCustody
from texturequest_dds_source import DDSStore
from texturequest_release import check_pause

from texturequest_version import APP_VERSION as VERSION
SCHEMA = 'texturequest-upgrade-v1'


class Upgrade:
    def __init__(self, state, graphics, loader=None):
        self.state = core._plain(state)
        self.graphics, self.loader = graphics, loader

    def receipt(self, game):
        key = hashlib.sha256(str(core._plain(game)).casefold().encode()).hexdigest()[:24]
        return core._plain(self.state / 'upgrades' / (key + '-' + VERSION + '.json'), self.state)

    def installed_game(self):
        """Recover a single old Replace install without asking for its folder again."""
        games = set()
        root = core._plain(self.state / 'games', self.state)
        if not root.exists(): return None
        paths = list(root.glob('*/manifest.json'))
        if len(paths) > 1000: return None
        for path in paths:
            try:
                value = core._load(core._plain(path, root))
                game = core._plain(value['game_dir'])
                manager = DirectSwapper(self.state)
                paks, expected = manager._game_manifest(game)
                if expected != path: continue
                manager._manifest(paks, expected)
                games.add(str(game))
            except (OSError, ValueError, KeyError, TypeError):
                continue
        return next(iter(games)) if len(games) == 1 else None

    def needed(self, game):
        if not str(game).strip(): return False
        path = self.receipt(game)
        if path.exists():
            value = core._load(path)
            if value.get('schema') != SCHEMA or value.get('game') != str(core._plain(game)):
                raise ValueError('Unknown upgrade receipt; existing files were preserved.')
            if value.get('status') == 'complete': return False
        manager = DirectSwapper(self.state)
        _, old = manager._game_manifest(game)
        dds = self.state / 'dds-prepared.json'
        old_dds = dds.exists() and core._load(dds).get('distribution') != 'generated-channels-only'
        return path.exists() or old.exists() or old_dds or self.graphics.status(game)['installed'] or bool(
            self.loader and self.loader.status(game)['installed']) or any(
                (self.state / name).exists() for name in ('direct-prepared.json', 'texturequest-prepared.json'))

    def _stock(self, game):
        """Only test the exact saved replacement identities, not guessed assets."""
        manager = DirectSwapper(self.state)
        paks, path = manager._game_manifest(game)
        value = manager._manifest(paks, path)
        replaced = []
        for row in value['archives']:
            if row.get('original_sha256') == row.get('sha256'): continue
            target = core._plain(paks / row['name'], paks)
            if not target.exists(): continue
            with DirectoryCustody(paks), FileCustody(target) as held:
                if held.matches(row): replaced.append(row['name'])
        if replaced:
            # Historical installs may have read-only attributes. The normal
            # uninstall unlocks owned archives; it does not replace their bytes.
            if any(row.get('managed') for row in value['archives']): manager.uninstall(game)
            raise ValueError('One-time upgrade: run the official EQ2 launcher → Advanced Tools → '
                             'Validate Game Assets to restore original PAKs. Close EQ2 and its launcher, '
                             'then click Finish upgrade here. Your custom textures and downloads are kept.')

    def before(self, game, report=lambda _: None):
        ensure_closed()
        path = self.receipt(game)
        path.parent.mkdir(parents=True, exist_ok=True)
        prior = core._load(path).get('previous_dds') if path.exists() else None
        pointer = self.state / 'dds-prepared.json'
        if pointer.exists():
            value = core._load(pointer)
            if value.get('distribution') != 'generated-channels-only': prior = value
        core._save(path, dict(schema=SCHEMA, game=str(core._plain(game)), version=VERSION,
                             status='preparing', previous_dds=prior, created_at=core._now()))
        self._stock(game)
        if self.loader:
            status = self.loader.status(game)
            if status['installed'] or status['running']:
                report('Removing the old background loader…')
                self.loader.uninstall(game, progress=report)
            deadline = time.monotonic() + 10
            while self.loader.cache_in_use():
                if time.monotonic() >= deadline:
                    raise ValueError('The old loader did not close. Close its game sessions, then click Finish upgrade.')
                time.sleep(.05)
        status = self.graphics.status(game)
        if status['installed'] or status['running']:
            report('Replacing the old graphics hook…')
            result = self.graphics.uninstall(game, progress=report)
            if any(name.casefold() != 'reshade.ini' for name in result.get('kept', [])):
                raise ValueError('Changed hook files were preserved. Remove the conflicting files listed in the install status, then click Finish upgrade.')

    def complete(self, game, prepared, report=lambda _: None, pause=None):
        """Reclaim only exact old cache bytes after new installation succeeds."""
        ensure_closed()
        path = self.receipt(game)
        path.parent.mkdir(parents=True, exist_ok=True)
        result = dict(schema=SCHEMA, game=str(core._plain(game)), version=VERSION,
                      status='cleaning', files=[], created_at=core._now())
        previous_dds = core._load(path).get('previous_dds') if path.exists() else None
        cleanup = self.state / 'cache-migrations' / (uuid.uuid4().hex + '-graphics-only.json')
        core._save(cleanup, result)
        with core._lock(self.state / 'operation.lock'), core._lock(self.state / 'sideload/helper-owner.lock'):
            ensure_closed()
            if self.graphics.legacy_in_use() or (self.loader and self.loader.cache_in_use()):
                raise ValueError('An old hook still uses the cache; saved files were preserved.')
            store = DDSStore(self.state)
            store.verify(prepared)
            if core._load(self.state / 'dds-prepared.json') != prepared or prepared.get('distribution') != 'generated-channels-only':
                raise ValueError('The new channel cache changed; old files were preserved.')
            other_games = [g for g in self.graphics._all_games()
                           if str(core._plain(g)).casefold() != str(core._plain(game)).casefold()
                           and self.graphics.status(g)['installed']]
            if other_games:
                result.update(status='complete', cache_preserved='Another installed game still uses this shared cache.',
                              completed_at=core._now(), cleanup=str(cleanup))
                core._save(cleanup, result); core._save(path, result)
                report('Upgrade complete. Shared cache kept for your other game installation.')
                return result
            direct = DirectSwapper(self.state)
            previous = None
            old = self.state / 'direct-prepared.json'
            if old.exists():
                try: previous = direct._prepared(core._load(old))
                except (ValueError, OSError, KeyError, TypeError): pass
            try:
                candidates = _obsolete_archives(direct, {'archives': []}, previous)
            except (OSError, ValueError, KeyError, TypeError) as error:
                candidates = []
                result['old_paks_preserved'] = str(error)
            downloads = core._plain(self.state / 'downloads', self.state)
            for package in LEGACY_PACKAGES:
                for candidate in (downloads / package['name'], downloads / package['version'] / package['name']):
                    if candidate.exists(): candidates.append((candidate, package, 'old-package', None))
            if previous_dds:
                # The old published index supplies complete member hashes; a
                # familiar ZIP name alone never authorizes deletion.
                try:
                    digest = previous_dds['index_sha256']
                    if not re.fullmatch('[a-f0-9]{64}', digest): raise ValueError('Old index identity')
                    metadata = core._plain(self.state / 'dds/metadata' / (digest + '.json'), self.state)
                    if metadata.stat().st_size > 128 * 1024**2: raise ValueError('Old index size')
                    raw = metadata.read_bytes()
                    if hashlib.sha256(raw).hexdigest() != digest: raise ValueError('Old index checksum')
                    index = core._json(raw)
                    if index.get('schema') != 'texturequest-dds-index-v1': raise ValueError('Old index schema')
                    chunks = {}
                    for row in index['objects']:
                        if row['entry'] != 'dds/' + row['sha256'] + '.dds': raise ValueError('Old DDS identity')
                        chunks.setdefault(row['chunk'], {})[row['entry']] = row
                    for name, members in chunks.items():
                        if not re.fullmatch(r'dds-\d{3}-[a-f0-9]{12}\.zip', name): raise ValueError('Old DDS chunk name')
                        candidate = core._plain(self.state / 'dds/downloads' / name, self.state)
                        if not candidate.exists(): continue
                        with DirectoryCustody(candidate.parent), FileCustody(candidate) as held:
                            digest = hashlib.file_digest(held.stream, 'sha256').hexdigest()
                            if digest[:12] != name[-16:-4]: raise ValueError('Old DDS ZIP checksum')
                            held.stream.seek(0)
                            with zipfile.ZipFile(held.stream) as archive:
                                infos = archive.infolist()
                                if len(infos) != len(members) or {x.filename for x in infos} != set(members):
                                    raise ValueError('Old DDS ZIP members')
                                for info in infos:
                                    check_pause(pause)
                                    pin = members[info.filename]
                                    if info.file_size != pin['bytes']: raise ValueError('Old DDS member size')
                                    with archive.open(info) as stream:
                                        if hashlib.file_digest(stream, 'sha256').hexdigest() != pin['sha256']:
                                            raise ValueError('Old DDS member checksum')
                            candidates.append((candidate, dict(bytes=candidate.stat().st_size, sha256=digest), 'old-dds-download', None))
                except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
                    result['old_dds_preserved'] = str(error)
            # Old DDS objects are content addressed. Keep every new referenced
            # object and reclaim only other correctly named, verified objects.
            wanted = {row['sha256'] for row in store.object_rows(prepared)}
            objects = core._plain(self.state / 'dds/objects/dds', self.state)
            if objects.exists():
                for candidate in objects.iterdir():
                    if re.fullmatch('[a-f0-9]{64}\\.dds', candidate.name) and candidate.stem not in wanted:
                        candidates.append((candidate, dict(bytes=candidate.stat().st_size, sha256=candidate.stem), 'obsolete-dds', None))
            unique = {str(p).casefold(): (p, pin, kind, owner) for p, pin, kind, owner in candidates}
            for candidate, pin, kind, owner in unique.values():
                check_pause(pause)
                item = dict(path=str(candidate), bytes=pin['bytes'], sha256=pin['sha256'], kind=kind, status='planned')
                result['files'].append(item); core._save(cleanup, result)
                try:
                    candidate = core._plain(candidate, self.state)
                    ensure_closed()
                    with DirectoryCustody(candidate.parent), ExitStack() as stack:
                        if owner:
                            stack.enter_context(FileCustody(owner['path'])).checked(dict(owner, name=Path(owner['path']).name), pause)
                        held = stack.enter_context(FileCustody(candidate))
                        held.checked(dict(pin, name=candidate.name), pause)
                        check_pause(pause)
                        held.remove()
                    item['status'] = 'removed'
                except (OSError, ValueError) as error:
                    item.update(status='preserved', reason=str(error))
                core._save(cleanup, result)
            result.update(status='complete', completed_at=core._now(), cleanup=str(cleanup))
            core._save(cleanup, result)
            core._save(path, result)
        report('Upgrade complete. Verified obsolete caches removed; custom textures, settings and unknown files kept.')
        return result
