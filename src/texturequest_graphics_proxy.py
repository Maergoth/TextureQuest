"""Install the TextureQuest add-on for stock ReShade; never inject into or launch the game.

The game folder receives the add-on, its TextureQuest.ini, and, only when no
ReShade is present, the official ReShade add-on build as dxgi.dll.
"""
from pathlib import Path
import configparser
import hashlib
import os
import re
import time
import uuid

from texture_swapper import core
from texturequest_sideload import LoaderManager, _pin, _json, _alive, _live_status_bytes, _remove_run, _signal_stop
from texturequest_processes import ensure_closed
from texturequest_shortcut import normalize, label
from texturequest_texture_map import activate_map, discard_map, fetch_map, stage_map
import texturequest_graphics_catalog as catalog

RUNTIME_SCHEMA = 'texturequest-graphics-runtime-v3'
RECORD_SCHEMA = 'texturequest-graphics-addon-installation-v1'
PROXY_NAMES = ('dxgi.dll', 'd3d11.dll')
STANDARD_MARKER = b'only limited add-on functionality'
RESHADE_MARKER = b'ReShadeRegisterAddon'
PROVIDER_170_SHA256 = '239005eae82cbf4a14e7c06d44448dd426dedc2e826896b3369e289b8398f168'
DEFAULT_RESHADE_INI = b'[GENERAL]\r\nNoReloadOnInit=1\r\n[INPUT]\r\nKeyOverlay=0,0,0,0\r\n[OVERLAY]\r\nTutorialProgress=4\r\n'


def need(ok, why):
    if not ok:
        raise ValueError(why)


def exclusive(path, value):
    with core._plain(path).open('xb') as f:
        f.write(catalog.canonical(value)+b'\n'); f.flush(); os.fsync(f.fileno())


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def classify(path):
    """'none', 'addon', 'standard' or 'foreign' for a proxy DLL in the game folder."""
    if not path.is_file():
        return 'none'
    data = path.read_bytes()
    if RESHADE_MARKER not in data or _sha(data) == PROVIDER_170_SHA256:
        return 'foreign'
    return 'standard' if STANDARD_MARKER in data else 'addon'


class GraphicsManager:
    def __init__(self, state_dir, *, runtime_dir=None):
        self.state_dir = core._plain(state_dir)
        self.runtime_dir = Path(runtime_dir) if runtime_dir else Path(__file__).resolve().parent / 'graphics-runtime'
        self.base = self.state_dir / 'graphics'
        self.records = self.state_dir / 'graphics-addon'
        self.proxy_records = self.state_dir / 'graphics-proxy'

    def _runtime(self):
        path = self.runtime_dir / 'RUNTIME.json'
        if not path.exists():
            return None
        doc = _json(path)
        need(doc.get('schema') == RUNTIME_SCHEMA and doc.get('reshade_build') == 'addon', 'Unknown graphics runtime.')
        names = {row['name'] for row in doc['files']}
        need(doc.get('addon') in names and doc.get('reshade') in names, 'Graphics runtime does not pin its add-on and ReShade.')
        for row in doc['files']:
            _pin(self.runtime_dir / row['name'], row)
        return doc

    def _key(self, game):
        return hashlib.sha256(str(core._plain(game)).casefold().encode()).hexdigest()[:24]

    def _record_path(self, game):
        return self.records / (self._key(game) + '.json')

    def _legacy_path(self, game): return self.base/'registrations'/(self._key(game)+'.json')

    def _read_legacy(self, path, game):
        raw = _live_status_bytes(path)
        if raw is None: return None
        d = core._json(raw)
        need(d.get('record_sha256')==core._digest({k:v for k,v in d.items() if k!='record_sha256'}) and
             str(core._plain(d['game'])).casefold()==str(core._plain(game)).casefold(), 'Graphics record changed; preserve it for recovery.')
        need(d.get('schema')=='texturequest-graphics-installation-v1' and
             re.fullmatch('[a-f0-9]{32}',d.get('installation_id','')) is not None,
             'Unknown legacy graphics record.')
        return d

    def _legacy(self, game): return self._read_legacy(self._legacy_path(game),game)

    def _record(self, game):
        path = self._record_path(game)
        if not path.exists():
            return None
        doc = _json(path)
        need(doc.get('schema') == RECORD_SCHEMA and doc.get('record_sha256') == core._digest({k: v for k, v in doc.items() if k != 'record_sha256'}),
             'Graphics record changed; preserve it for recovery.')
        return doc

    def _save(self, game, doc):
        doc['record_sha256'] = core._digest({k: v for k, v in doc.items() if k != 'record_sha256'})
        self.records.mkdir(parents=True, exist_ok=True)
        core._atomic(self._record_path(game), doc)

    def _games_open(self, game):
        import psutil
        target = str(core._plain(game) / 'EverQuest2.exe').casefold()
        count = 0
        for p in psutil.process_iter(['pid', 'name']):
            if (p.info['name'] or '').casefold() == 'everquest2.exe':
                try:
                    count += p.exe().casefold() == target
                except psutil.NoSuchProcess:
                    pass
        return count

    def _addon_folder(self, game):
        ini = game / 'ReShade.ini'
        if not ini.is_file():
            return game
        parser = configparser.ConfigParser(strict=False, interpolation=None)
        try:
            parser.read_string(ini.read_text(encoding='utf-8-sig', errors='replace'))
        except configparser.Error as error:
            raise ValueError('ReShade.ini could not be read: ' + str(error)) from error
        value = parser.get('ADDON', 'AddonPath', fallback='').strip()
        if not value:
            return game
        folder = Path(value) if Path(value).is_absolute() else game / value
        need(folder.is_dir(), 'ReShade AddonPath folder does not exist: ' + value)
        return core._plain(folder)

    def _write_key(self, binding):
        self.base.mkdir(parents=True, exist_ok=True)
        raw = ('TQKEY1\n' + str(binding['vk']) + '\n' + str(binding['modifiers']) + '\n').encode()
        tmp = self.base / ('shortcut-' + uuid.uuid4().hex + '.tmp')
        with tmp.open('xb') as f:
            f.write(raw)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.base / 'shortcut.txt')

    def _addon_ini(self, game, map_path):
        lines = ['[TextureQuest]', 'Map=' + str(map_path), 'Downloads=' + str(self.state_dir / 'downloads'),
                 'Shortcut=' + str(self.base / 'shortcut.txt'), 'GamePaks=' + str(game / 'paks'), '']
        return '\r\n'.join(lines).encode('utf-8')

    def status(self, game):
        runtime = self._runtime()
        result = dict(installed=False, running=False, active_sessions=0, available=runtime is not None,
                      dual_load=False, shortcut=normalize(), message='Graphics hook runtime is not included.')
        if not str(game).strip():
            return result
        record = self._record(game)
        if record and record['phase'] != 'uninstalled':
            binding = normalize(record.get('shortcut'))
            missing = [Path(x['path']).name for x in record['files'] if not Path(x['path']).is_file()]
            message = ('Interrupted graphics installation. Close EQ2 and Uninstall before retrying.' if record['phase'] != 'installed' else
                       'Hook file missing (' + ', '.join(missing) + '). Close EQ2, then Uninstall and Install to repair.' if missing else
                       'Graphics hook installed as a ReShade add-on. ' + label(binding) + ' switches quality.')
            return dict(result, installed=True, active_sessions=self._games_open(game), available=True, shortcut=binding, message=message)
        if self._proxy_170(game):
            return dict(result, installed=True, active_sessions=self._games_open(game), available=True,
                        message='TextureQuest 1.7.0 graphics hook. Close EQ2 and Uninstall here, then Install the add-on.')
        legacy = self._legacy(game)
        if legacy:
            sessions = legacy.get('sessions', [])
            count = sum(bool(_alive(x, x['exe'])) for x in sessions)
            watcher = legacy.get('watcher')
            running = bool(watcher and _alive(watcher, Path(legacy['runtime_dir']) / 'TextureQuestGraphics.exe'))
            if legacy['phase'] != 'uninstalled' or running or count:
                return dict(result, installed=legacy['phase'] != 'uninstalled', running=running, active_sessions=count,
                            available=True, message='Old injected graphics hook. Close EQ2 and Uninstall here, then Install the add-on.')
        if runtime:
            result['message'] = 'Upscaled textures through a ReShade add-on. ' + label() + ' switches Original/Upscaled. Close EQ2 to install.'
        return result

    def _all_games(self):
        found = {}
        for folder in [self.records, self.proxy_records, self.base / 'registrations']:
            if not folder.exists():
                continue
            for p in folder.glob('*.json'):
                game = _json(p)['game']
                found[str(core._plain(game)).casefold()] = game
        return list(found.values())

    def cache_in_use(self):
        return any(any(self.status(g)[k] for k in ['installed', 'running', 'active_sessions']) for g in self._all_games())

    def _addon_installed(self, game):
        record = self._record(game)
        return bool(record and record['phase'] != 'uninstalled')

    def legacy_in_use(self):
        """Whether a 1.7.0 or injected hook is installed or running; those depend on app and cache files."""
        return any(any(self.status(g)[k] for k in ['installed', 'running', 'active_sessions'])
                   for g in self._all_games() if not self._addon_installed(g))

    def downloads_in_use(self):
        """Whether the downloaded archives are in use: EQ2 is running with the add-on, or an older hook is installed."""
        return self.legacy_in_use() or any(self.status(g)['active_sessions'] for g in self._all_games() if self._addon_installed(g))

    def stage_map(self, manifest):
        """When the add-on is installed, download and verify the map for this release before its archives change.

        Raises when the map is unavailable, so the update stops while the current archives and map still match.
        Returns the staged map for activate_map; None when there is nothing to activate."""
        if not any(self._addon_installed(g) for g in self._all_games()):
            return None
        with core._lock(self.state_dir / 'operation.lock'):
            return stage_map(self.state_dir, core._digest(manifest))

    def activate_map(self, staged):
        """Make the staged map active once the matching archives are in place."""
        with core._lock(self.state_dir / 'operation.lock'):
            activate_map(self.state_dir, staged)

    def discard_map(self, staged):
        discard_map(staged)

    def installed_game(self):
        games = [g for g in self._all_games() if self.status(g)['installed']]
        need(len(games) <= 1, 'Multiple graphics installations; choose a game folder explicitly.')
        return games[0] if games else None

    def _exclusive_mode(self, game):
        need(not LoaderManager(self.state_dir).cache_in_use(), 'Uninstall the old Memory hook and close its sessions first.')
        legacy = self._legacy(game)
        need(not legacy or (legacy['phase'] == 'uninstalled' and not self.status(game)['running'] and
             not self.status(game)['active_sessions']), 'Uninstall the old injected graphics hook first.')

    def _proxy_170_path(self, game):
        return self.proxy_records / (self._key(game) + '.json')

    def _proxy_170(self, game):
        """The 1.7.0 graphics proxy record (d3d11.dll + TextureQuest.ini) unless absent or uninstalled."""
        path = self._proxy_170_path(game)
        if not path.exists():
            return None
        doc = _json(path)
        need(doc.get('schema') == 'texturequest-graphics-proxy-installation-v1' and
             doc.get('record_sha256') == core._digest({k: v for k, v in doc.items() if k != 'record_sha256'}),
             'Graphics record changed; preserve it for recovery.')
        need(type(doc.get('files')) is list and len(doc['files']) == 2 and
             {str(core._plain(x['path'])).casefold() for x in doc['files']} ==
             {str(core._plain(game) / n).casefold() for n in ['d3d11.dll', 'TextureQuest.ini']}, 'Proxy file scope changed.')
        return None if doc.get('phase') == 'uninstalled' else doc

    def _uninstall_proxy_170(self, game):
        """Remove a 1.7.0 graphics proxy install through its own record; return the changed files it kept."""
        doc = self._proxy_170(game)
        if not doc:
            return []
        kept = []
        for row in sorted(doc['files'], key=lambda x: 0 if x['path'].lower().endswith('.dll') else 1):
            p = core._plain(row['path'])
            if not p.exists():
                continue
            if _sha(p.read_bytes()) != row['sha256']:
                kept.append(p.name)
                continue
            ensure_closed()
            p.unlink()
        doc['phase'] = 'uninstalled'
        doc['kept'] = kept
        doc['record_sha256'] = core._digest({k: v for k, v in doc.items() if k != 'record_sha256'})
        core._atomic(self._proxy_170_path(game), doc)
        return kept

    def install(self, game, prepared, *, dual_load=False, progress=None, shortcut=None):
        need(dual_load is False, 'Dual-load is not supported.')
        report = progress or (lambda _: None)
        binding = normalize(shortcut)
        game = core._plain(core.validate_game(game).parent)
        runtime = self._runtime()
        need(runtime, 'Graphics runtime missing.')
        ensure_closed()
        with core._lock(self.state_dir / 'operation.lock'), core._lock(self.state_dir / 'sideload/helper-owner.lock'):
            self._exclusive_mode(game)
            old = self._record(game)
            need(not old or old['phase'] == 'uninstalled', 'Uninstall Graphics hook before installing again.')
            standard = [name for name in PROXY_NAMES if classify(game / name) == 'standard']
            if standard:
                raise ValueError('ReShade ' + standard[0] + ' is the standard build, which cannot load add-ons. '
                                 'Install the ReShade add-on build (reshade.me, "with full add-on support") over it, then Install again. '
                                 'Your ReShade was left unchanged.')
            self._uninstall_proxy_170(game)
            kinds = {name: classify(game / name) for name in PROXY_NAMES}
            foreign = [name for name, kind in kinds.items() if kind == 'foreign']
            need(not foreign, 'An unknown graphics proxy is installed: ' + ', '.join(foreign) + '. Remove it first; it was left unchanged.')
            report('Downloading the texture map.')
            map_path = fetch_map(self.state_dir, prepared)
            self._write_key(binding)
            writes, owned = [], False
            if all(kind == 'none' for kind in kinds.values()):
                owned = True
                writes.append((game / 'dxgi.dll', (self.runtime_dir / runtime['reshade']).read_bytes()))
                if not (game / 'ReShade.ini').exists():
                    writes.append((game / 'ReShade.ini', DEFAULT_RESHADE_INI))
            folder = self._addon_folder(game)
            writes.append((folder / 'TextureQuest.addon64', (self.runtime_dir / runtime['addon']).read_bytes()))
            writes.append((folder / 'TextureQuest.ini', self._addon_ini(game, map_path)))
            for path, _ in writes:
                need(not path.exists(), path.name + ' already exists. It was left unchanged.')
            files = [dict(path=str(p), bytes=len(d), sha256=_sha(d)) for p, d in writes]
            doc = dict(schema=RECORD_SCHEMA, phase='installing', id=uuid.uuid4().hex, game=str(game), files=files,
                       reshade_owned=owned, shortcut=binding, map=str(map_path))
            self._save(game, doc)
            for path, data in writes:
                ensure_closed()
                tmp = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
                try:
                    with tmp.open('xb') as f:
                        f.write(data)
                        f.flush()
                        os.fsync(f.fileno())
                    # os.rename refuses an existing target on Windows, so a new file never overwrites.
                    os.rename(tmp, path)
                finally:
                    tmp.unlink(missing_ok=True)
            doc['phase'] = 'installed'
            self._save(game, doc)
        report('Graphics hook installed. ' + label(binding) + ' switches quality.')
        return self.status(game)

    def _uninstall_legacy(self, game, record):
        need(record['stop_event']=='Local\\TQWatch-'+record['installation_id'] and
             record.get('startup_name')=='TextureQuestGraphics-'+self._key(game),'Legacy watcher ownership changed.')
        with core._lock(self.base/'record.lock'):
            fresh=self._legacy(game);need(fresh==record,'Legacy graphics record changed.')
            _remove_run(record['startup_name'],record['startup_command'])
            watcher=record.get('watcher')
            if watcher and _alive(watcher,Path(record['runtime_dir'])/'TextureQuestGraphics.exe'):_signal_stop(record['stop_event'])
            history=core._plain(Path(record['home'])/'record-history');history.mkdir(exist_ok=True)
            exclusive(history/(uuid.uuid4().hex+'.json'),record)
            record['phase']='uninstalled';record['watch_state']='stopped'
            record['record_sha256']=core._digest({k:v for k,v in record.items() if k!='record_sha256'})
            core._atomic(self._legacy_path(game),record)
        end=time.monotonic()+10
        while watcher and _alive(watcher,Path(record['runtime_dir'])/'TextureQuestGraphics.exe'):
            need(time.monotonic()<end,'Legacy watcher stop not confirmed. Preserve its files.');time.sleep(.05)

    def uninstall(self, game, progress=None):
        game = core._plain(core.validate_game(game).parent)
        ensure_closed()
        kept = []
        with core._lock(self.state_dir / 'operation.lock'), core._lock(self.state_dir / 'sideload/helper-owner.lock'):
            doc = self._record(game)
            if not doc or doc['phase'] == 'uninstalled':
                if self._proxy_170(game):
                    kept = self._uninstall_proxy_170(game)
                    message = 'Graphics hook removed.' + (' Changed files kept: ' + ', '.join(kept) + '.' if kept else '')
                    if progress:
                        progress(message)
                    return dict(self.status(game), message=message)
                legacy = self._legacy(game)
                need(legacy and legacy['phase'] != 'uninstalled', 'No installed graphics hook.')
                self._uninstall_legacy(game, legacy)
                if progress:
                    progress('Old graphics hook removed.')
                return self.status(game)
            for row in doc['files']:
                p = core._plain(row['path'])
                if not p.exists():
                    continue
                if p.name.casefold() in PROXY_NAMES and not doc['reshade_owned']:
                    continue
                if _sha(p.read_bytes()) != row['sha256']:
                    kept.append(p.name)
                    continue
                ensure_closed()
                p.unlink()
            doc['phase'] = 'uninstalled'
            doc['kept'] = kept
            self._save(game, doc)
        message = 'Graphics hook removed.' + (' Changed files kept: ' + ', '.join(kept) + '.' if kept else '')
        if progress:
            progress(message)
        return dict(self.status(game), message=message)

    def set_shortcut(self, game, shortcut, progress=None):
        binding = normalize(shortcut)
        game = core._plain(game)
        with core._lock(self.state_dir / 'operation.lock'):
            doc = self._record(game)
            need(doc and doc['phase'] == 'installed', 'Install Graphics hook before saving its shortcut.')
            self._write_key(binding)
            doc['shortcut'] = binding
            self._save(game, doc)
        if progress:
            progress('Shortcut saved: ' + label(binding) + '. Active only while EQ2 is focused.')
        return self.status(game)
