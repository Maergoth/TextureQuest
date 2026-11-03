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
from texturequest_dds_source import DDSStore
import texturequest_graphics_catalog as catalog
from texturequest_hotkeys import sanitize_reshade_config, sanitize_reshade_preset
from texturequest_components import normalize as component_selection
from texturequest_game_settings import GameSettings
from texturequest_version import APP_VERSION
from texturequest_latest import version_number

try:
    from texturequest_build_pin import APP_VERSION
except ImportError:
    pass

RUNTIME_SCHEMA = 'texturequest-graphics-runtime-v4'
RENDERER_PROFILE_ID = 'sichii-4a656a9-texturequest-master-v1'
RENDERER_FILE = 'renderer/EQ2Graphics.addon64'
RENDERER_MODULE = 'EQ2Graphics.addon64'
RECORD_SCHEMA = 'texturequest-graphics-addon-installation-v1'
PROXY_NAMES = ('dxgi.dll', 'd3d11.dll')
STANDARD_MARKER = b'only limited add-on functionality'
RESHADE_MARKER = b'ReShadeRegisterAddon'
PROVIDER_170_SHA256 = '239005eae82cbf4a14e7c06d44448dd426dedc2e826896b3369e289b8398f168'
DEFAULT_RESHADE_INI = sanitize_reshade_config(b'[GENERAL]\r\nNoReloadOnInit=1\r\n[INPUT]\r\nKeyOverlay=36,0,0,0\r\n[OVERLAY]\r\nTutorialProgress=4\r\nShowFPS=0\r\nShowClock=0\r\nShowFrameTime=0\r\nShowPresetName=0\r\nShowPresetTransitionMessage=0\r\n')
DLSS_PROFILE_ID = 'feeder-renodx-10.8.0-nr310.8.0-sr310.9.16-lumenite3-private-v1'
# One reviewed private graph. No discovery of arbitrary downloaded DLLs or
# scripts, and no public-redistribution claim is implied by its availability.
DLSS_ROLES = {
    'feeder': 'dlss/dlss5-feed.addon64', 'consumer': 'dlss/renodx-dlss5.addon64',
    'sr': 'dlss/nvngx_dlss.dll', 'nr': 'dlss/nvngx_dlssnr.dll',
    'feed_shader': 'dlss/shaders/DLSS5_Feed.fx',
    'motion_shader': 'dlss/shaders/lumenite_Kernel.fx',
    'reshade_include': 'dlss/shaders/ReShade.fxh',
    'motion_color': 'dlss/shaders/include/lumenite_ColorManagement.fxh',
    'motion_compute': 'dlss/shaders/include/lumenite_Compute.fxh',
    'motion_helpers': 'dlss/shaders/include/lumenite_Helpers.fxh',
    'motion_projections': 'dlss/shaders/include/lumenite_Projections.fxh',
}
DLSS_MODULES = ('feeder', 'consumer', 'sr', 'nr')
AIO_PROFILE_ID = 'aio-09301f5-managed-nvof-nr-sr-fg-private-v1'
AIO_V2_PROFILE_ID = 'aio-09301f5-managed-controls-mfg-private-v2'
AIO_V3_PROFILE_ID = 'aio-09301f5-managed-continuous-gui-private-v3'
AIO_V4_PROFILE_ID = 'aio-09301f5-managed-completion-cadence-private-v4'
AIO_V5_PROFILE_ID = 'aio-09301f5-managed-refresh-gui-ring-private-v5'
AIO_V6_PROFILE_ID = 'aio-09301f5-managed-capture-ahead-private-v6'
AIO_V7_PROFILE_ID = 'aio-09301f5-managed-latest-capture-private-v7'
AIO_ROLES = {
    'aio': 'dlss/standalone-dlssnr.addon64', 'bridge': 'dlss/nvngx.dll',
    'nr': 'dlss/nvngx_dlssnr.dll', 'sr': 'dlss/nvngx_dlss.dll', 'fg': 'dlss/nvngx_dlssg.dll',
    'feed_shader': 'dlss/shaders/DLSS5_AIO_Feed.fx', 'reshade_include': 'dlss/shaders/ReShade.fxh',
}
AIO_MODULES = ('aio', 'bridge', 'nr', 'sr', 'fg')
DLSS_PROFILES = {
    DLSS_PROFILE_ID: dict(backend='feeder', roles=DLSS_ROLES, modules=DLSS_MODULES),
    AIO_PROFILE_ID: dict(backend='aio', roles=AIO_ROLES, modules=AIO_MODULES),
    AIO_V2_PROFILE_ID: dict(backend='aio', roles=AIO_ROLES, modules=AIO_MODULES, settings='aio2'),
    AIO_V3_PROFILE_ID: dict(backend='aio', roles=AIO_ROLES, modules=AIO_MODULES, settings='aio2'),
    AIO_V4_PROFILE_ID: dict(backend='aio', roles=AIO_ROLES, modules=AIO_MODULES, settings='aio2'),
    AIO_V5_PROFILE_ID: dict(backend='aio', roles=AIO_ROLES, modules=AIO_MODULES, settings='aio2'),
    AIO_V6_PROFILE_ID: dict(backend='aio', roles=AIO_ROLES, modules=AIO_MODULES, settings='aio2'),
    AIO_V7_PROFILE_ID: dict(backend='aio', roles=AIO_ROLES, modules=AIO_MODULES, settings='aio2'),
}


def _dlss_contract(profile):
    identity = profile.get('id') if type(profile) is dict else None
    need(type(identity) is str and identity in DLSS_PROFILES, 'Unknown experimental DLSS profile.')
    return DLSS_PROFILES[identity]


def quiet_reshade(raw):
    """Hide persistent tutorial/stats and reset configurable ReShade shortcuts."""
    need(len(raw) <= 4 * 1024**2, 'ReShade configuration is too large.')
    text = raw.decode('utf-8-sig')
    parser = configparser.ConfigParser(strict=False, interpolation=None)
    parser.read_string(text)
    values = dict(TutorialProgress='4', ShowFPS='0', ShowClock='0', ShowFrameTime='0',
                  ShowPresetName='0', ShowPresetTransitionMessage='0')
    newline = '\r\n' if '\r\n' in text else '\n'
    lines, section, seen, found = [], '', set(), False
    for line in text.splitlines():
        match = re.match(r'\s*\[([^]]+)\]', line)
        if match:
            if section == 'overlay':
                lines.extend(k + '=' + v for k, v in values.items() if k not in seen)
            section = match[1].casefold()
            found |= section == 'overlay'
        key = re.match(r'\s*([^=;#]+?)\s*=', line)
        name = next((k for k in values if key and k.casefold() == key[1].casefold()), None)
        if section == 'overlay' and name:
            line = name + '=' + values[name]
            seen.add(name)
        if section == 'input' and key and key[1].casefold() == 'keyoverlay' and re.fullmatch(r'\s*0(?:\s*,\s*0){3}\s*', line.split('=', 1)[1]):
            line = 'KeyOverlay=36,0,0,0'
        lines.append(line)
    if section == 'overlay':
        lines.extend(k + '=' + v for k, v in values.items() if k not in seen)
    elif not found:
        lines.extend(['', '[OVERLAY]'] + [k + '=' + v for k, v in values.items()])
    result = (newline.join(lines) + newline).encode('utf-8')
    result = (b'\xef\xbb\xbf' + result) if raw.startswith(b'\xef\xbb\xbf') else result
    return sanitize_reshade_config(result)


def _preset_paths(raw, game):
    parser = configparser.ConfigParser(strict=False, interpolation=None)
    parser.read_string(raw.decode('utf-8-sig'))
    values = [parser.get('GENERAL', key, fallback='').replace(',,', ',') for key in ('PresetPath', 'StartupPresetPath')]
    # ReShade escapes literal commas by doubling them in string-list values.
    pending, current = [], ''
    value = parser.get('GENERAL', 'PresetShortcutPaths', fallback='')
    pos = 0
    while pos < len(value):
        if value[pos:pos+2] == ',,': current += ','; pos += 2
        elif value[pos] == ',': pending.append(current); current = ''; pos += 1
        else: current += value[pos]; pos += 1
    if current: pending.append(current)
    values += pending
    need(len(values) <= 64, 'Too many ReShade preset paths; settings preserved.')
    found = {}
    for value in values:
        value = value.strip().strip('"')
        if not value: continue
        path = Path(value) if Path(value).is_absolute() else game / value
        path = core._plain(path, game)
        if path.is_file(): found[str(path).casefold()] = path
    return list(found.values())


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
        self._dlss_verified = None

    def _runtime(self):
        path = self.runtime_dir / 'RUNTIME.json'
        if not path.exists():
            return None
        doc = _json(path)
        need(doc.get('schema') in (RUNTIME_SCHEMA, 'texturequest-graphics-runtime-v5', 'texturequest-graphics-runtime-v6', 'texturequest-graphics-runtime-v7', 'texturequest-graphics-runtime-v8') and doc.get('reshade_build') == 'addon' and
             doc.get('map_schema') == 'TQMAP002', 'Unknown graphics runtime.')
        names = {row['name'] for row in doc['files']}
        need(doc.get('addon') in names and doc.get('reshade') in names, 'Graphics runtime does not pin its add-on and ReShade.')
        effects = doc.get('effects')
        if effects is not None:
            bloom = dict(
                schema='texturequest-effects-runtime-v1', shader='effects/TQEffects.fx',
                effect_name='TQEffects.fx', technique='TQBloom')
            legacy_suite = dict(schema='texturequest-effects-runtime-v2', shader='effects/TQEffects.fx',
                         effect_name='TQEffects.fx', techniques=['TQAmbientOcclusion', 'TQContactShadows',
                         'TQIndirectLighting', 'TQBloom', 'TQAntiAliasing', 'TQDepthDiagnostic'])
            previous_suite = dict(schema='texturequest-effects-runtime-v3', shader='effects/TQEffects.fx',
                         effect_name='TQEffects.fx', techniques=['TQAmbientOcclusion', 'TQContactShadows',
                         'TQIndirectLighting', 'TQAntiAliasing', 'TQDepthDiagnostic'])
            suite = dict(schema='texturequest-effects-runtime-v4', shader='effects/TQEffects.fx',
                         effect_name='TQEffects.fx', techniques=['TQAmbientOcclusion', 'TQContactShadows',
                         'TQDepthDiagnostic'])
            need(((doc['schema'] == 'texturequest-graphics-runtime-v5' and effects == bloom) or
                  (doc['schema'] in ('texturequest-graphics-runtime-v6', 'texturequest-graphics-runtime-v7', 'texturequest-graphics-runtime-v8') and effects in (legacy_suite, previous_suite, suite))) and effects['shader'] in names,
                'Unknown optional effects runtime.')
        elif doc['schema'] in ('texturequest-graphics-runtime-v5', 'texturequest-graphics-runtime-v6', 'texturequest-graphics-runtime-v7'):
            raise ValueError('Graphics runtime is missing its effects contract.')
        need(doc['schema'] in ('texturequest-graphics-runtime-v7', 'texturequest-graphics-runtime-v8') or 'dlss' not in doc,
             'DLSS needs the versioned graphics runtime contract.')
        need(doc['schema'] == 'texturequest-graphics-runtime-v8' or 'renderer' not in doc,
             'The world renderer needs the versioned graphics runtime contract.')
        if doc['schema'] == 'texturequest-graphics-runtime-v8':
            need(effects is not None or doc.get('renderer') is not None,
                 'Graphics runtime is missing its graphical effects contract.')
        for row in doc['files']:
            _pin(self.runtime_dir / row['name'], row)
        return doc

    def _dlss_profile(self, runtime, *, full=False):
        """Verify optional bytes once for display, freshly for every mutation.

        The cache only avoids rehashing large model DLLs during GUI refresh.
        File identity/size/timestamps invalidate it; installation never trusts it.
        """
        profile = runtime.get('dlss')
        need(runtime.get('schema') in ('texturequest-graphics-runtime-v7', 'texturequest-graphics-runtime-v8') and type(profile) is dict,
             'The experimental DLSS profile is not included.')
        need(set(profile) == {'schema', 'id', 'private', 'api', 'files'} and
             profile['schema'] == 'texturequest-dlss-runtime-v1' and
             profile['private'] is True and profile['api'] == 'd3d11', 'Unknown experimental DLSS profile.')
        contract = _dlss_contract(profile)
        roles = contract['roles']
        rows = profile['files']
        need(type(rows) is list and len(rows) == len(roles), 'Incomplete DLSS role map.')
        seen, sources, fingerprints = set(), [], []
        for row in rows:
            need(type(row) is dict and set(row) == {'role', 'name', 'bytes', 'sha256'}, 'Unknown DLSS file fields.')
            role = row['role']
            need(type(role) is str and role in roles and role not in seen and row['name'] == roles[role],
                 'Unknown, duplicate or unsafe DLSS role.')
            need(type(row['bytes']) is int and 0 < row['bytes'] <= 256 * 1024**2 and
                 type(row['sha256']) is str and re.fullmatch('[a-f0-9]{64}', row['sha256']), 'Invalid DLSS file pin.')
            seen.add(role)
            source = core._plain(self.runtime_dir / row['name'], self.runtime_dir)
            stat = source.stat()
            need(source.is_file(), 'Missing experimental DLSS file: ' + source.name)
            sources.append((source, row))
            fingerprints.append((str(source), stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
        need(not ({row['name'].casefold() for row in rows} & {row['name'].casefold() for row in runtime['files']}),
             'DLSS roles collide with the core runtime.')
        key = (_sha(catalog.canonical(profile)), tuple(fingerprints))
        if full or self._dlss_verified != key:
            for source, row in sources: _pin(source, row)
            self._dlss_verified = key
        return profile

    def _renderer_profile(self, runtime):
        profile = runtime.get('renderer')
        need(runtime.get('schema') == 'texturequest-graphics-runtime-v8' and type(profile) is dict,
             'The world renderer is not included.')
        need(set(profile) == {'schema', 'id', 'private', 'api', 'files'} and
             profile['schema'] == 'texturequest-world-renderer-v1' and
             profile['id'] == RENDERER_PROFILE_ID and profile['private'] is True and
             profile['api'] == 'd3d11', 'Unknown world renderer profile.')
        rows = profile['files']
        need(type(rows) is list and len(rows) == 1, 'Incomplete world renderer files.')
        row = rows[0]
        need(type(row) is dict and set(row) == {'name', 'bytes', 'sha256'} and
             row['name'] == RENDERER_FILE and type(row['bytes']) is int and
             0 < row['bytes'] <= 32 * 1024**2 and type(row['sha256']) is str and
             re.fullmatch('[a-f0-9]{64}', row['sha256']), 'Invalid world renderer file pin.')
        need(row['name'].casefold() not in {item['name'].casefold() for item in runtime['files']},
             'World renderer collides with the core runtime.')
        _pin(core._plain(self.runtime_dir / row['name'], self.runtime_dir), row)
        return profile

    def _preflight_renderer(self, game, runtime):
        profile = self._renderer_profile(runtime)
        path = core._plain(self._addon_folder(game) / RENDERER_MODULE)
        if path.exists():
            record = self._record(game)
            owned = next((row for row in (record or {}).get('files', [])
                          if core._plain(row['path']) == path), None)
            need(record and record['phase'] != 'uninstalled' and
                 record.get('renderer_profile') == RENDERER_PROFILE_ID and owned and
                 _pin(path)['sha256'] == owned['sha256'],
                 'World renderer file already exists or changed: ' + path.name + '. It was kept.')
        return profile

    def _preflight_components(self, game, selection, runtime):
        renderer = selection['effects'] and runtime.get('renderer') is not None
        if renderer:
            self._preflight_renderer(game, runtime)
        else:
            need(not selection['effects'] or runtime.get('effects'), 'The optional effects runtime is not included.')
            need(not selection['effects'] or runtime['effects'].get('schema') == 'texturequest-effects-runtime-v4',
                 'The bundled effects are outdated.')
        if not selection['dlss']:
            self._preflight_optional_settings(game, selection, runtime, None)
            return None
        profile = self._dlss_profile(runtime, full=True)
        need(self._addon_folder(game) == game,
             'Experimental DLSS currently needs ReShade add-ons in the game folder. Your AddonPath was kept.')
        record = self._record(game)
        owned = {str(core._plain(row['path'])).casefold(): row for row in (record or {}).get('files', [])}
        contract = _dlss_contract(profile)
        targets = {Path(contract['roles'][role]).name.casefold() for role in contract['modules']}
        known_modules = {Path(info['roles'][role]).name.casefold()
                         for info in DLSS_PROFILES.values() for role in info['modules']}
        def removable(path):
            row = owned.get(str(path).casefold())
            return bool(record and record['phase'] != 'uninstalled' and path.name.casefold() in known_modules and
                        row and _pin(path)['sha256'] == row['sha256'])
        for name in targets:
            path = core._plain(game / name, game)
            if not path.exists(): continue
            need(removable(path),
                 'DLSS file already exists or changed: ' + path.name + '. It was kept.')
        addons = list(game.glob('*.addon64'))
        need(len(addons) <= 256, 'Too many add-ons for a bounded DLSS conflict check.')
        conflicts = [p.name for p in addons if p.name.casefold() not in targets and
                     any(word in p.name.casefold() for word in ('dlss', 'optiscaler', 'streamline')) and
                     not removable(core._plain(p, game))]
        for name in (known_modules - targets) | {'optiscaler.dll', 'sl.interposer.dll'}:
            path = core._plain(game / name, game)
            if path.exists() and not removable(path): conflicts.append(path.name)
        need(not conflicts, 'Another DLSS add-on is installed: ' + ', '.join(conflicts) + '. It was kept.')
        self._preflight_optional_settings(game, selection, runtime, profile)
        return profile

    def _preflight_optional_settings(self, game, selection, runtime, profile):
        """Exercise the same pure transforms without creating cache or settings."""
        from texturequest_effects import prepare_effects_config, retire_removed_effect_settings
        ini = core._plain(game / 'ReShade.ini', game)
        raw = quiet_reshade(ini.read_bytes()) if ini.exists() else DEFAULT_RESHADE_INI
        retire_removed_effect_settings(raw)
        for path in _preset_paths(raw, game):
            retire_removed_effect_settings(path.read_bytes())
        if not selection['effects'] and profile is None: return
        if selection['effects'] and runtime.get('renderer') is None:
            pin = next(row for row in runtime['files'] if row['name'] == runtime['effects']['shader'])
            directory = core._plain(self.state_dir / 'effects' / pin['sha256'], self.state_dir)
            raw, _ = prepare_effects_config(raw, str(directory))
        if profile is not None:
            from texturequest_dlss import prepare_dlss_config
            directory = core._plain(self.state_dir / 'dlss' / _sha(catalog.canonical(profile)), self.state_dir)
            raw, _ = prepare_effects_config(raw, str(directory))
            contract = _dlss_contract(profile)
            prepare_dlss_config(raw, profile=contract.get('settings', contract['backend']))

    def preflight_components(self, game, components=None):
        """Before Apply changes removes anything, verify selected optional graph."""
        selection = component_selection(components)
        game = core._plain(core.validate_game(game).parent)
        ensure_closed()
        with core._lock(self.state_dir / 'operation.lock'), core._lock(self.state_dir / 'sideload/helper-owner.lock'):
            runtime = self._runtime()
            need(runtime, 'Graphics runtime missing.')
            self._preflight_components(game, selection, runtime)
        return dict(verified=True, components=selection)

    def _dlss_shaders(self, profile):
        folder = core._plain(self.state_dir / 'dlss' / _sha(catalog.canonical(profile)), self.state_dir)
        modules = _dlss_contract(profile)['modules']
        for row in profile['files']:
            if row['role'] in modules: continue
            source = core._plain(self.runtime_dir / row['name'], self.runtime_dir)
            _pin(source, row)
            raw = source.read_bytes()
            need(len(raw) == row['bytes'] and _sha(raw) == row['sha256'], 'DLSS shader changed while reading.')
            relative = Path(row['name']).relative_to('dlss/shaders')
            target = core._plain(folder / relative, self.state_dir)
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                with target.open('xb') as stream:
                    stream.write(raw); stream.flush(); os.fsync(stream.fileno())
            _pin(target, row)
        return folder

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

    def _assert_shortcuts_settled(self, game):
        """Never mistake a missing interrupted settings file for a fresh install."""
        history = core._plain(self.state_dir / 'reshade-settings-history', self.state_dir)
        if not history.exists(): return
        journals = list(history.glob('*-shortcuts.json'))
        need(len(journals) <= 1024, 'Too many settings journals; settings preserved.')
        for path in journals:
            need(path.stat().st_size <= 4 * 1024**2, 'Settings journal is too large; settings preserved.')
            doc = _json(path)
            need(doc.get('schema') == 'texturequest-reshade-shortcuts-v1' and
                 doc.get('phase') in ('prepared', 'complete') and isinstance(doc.get('files'), list),
                 'Unknown settings journal; settings preserved: ' + str(path))
            if doc['phase'] == 'prepared':
                targets = [core._plain(row['path']) for row in doc['files']]
                need(not any(p.is_relative_to(game) for p in targets),
                     'An interrupted ReShade settings reset needs recovery. Original settings and backups are preserved: ' + str(path))

    def _repair_shortcuts(self, game, transform=None, *, reset_shortcuts=True):
        """Caller owns operation locks. Keep settings-only backups and a journal."""
        from contextlib import ExitStack
        from texturequest_direct import FileCustody
        from texturequest_custody import DirectoryCustody
        from texturequest_effects import retire_removed_effect_settings
        self._assert_shortcuts_settled(game)
        ini = core._plain(game / 'ReShade.ini', game)
        if not ini.exists(): return dict(changed=0, paths=[])
        before = ini.read_bytes()
        after = quiet_reshade(before) if reset_shortcuts else before
        if transform: after = transform(after)
        if reset_shortcuts: after = retire_removed_effect_settings(after)
        plan = [(ini, before, after)]
        for path in (_preset_paths(before, game) if reset_shortcuts else []):
            need(str(path).casefold() != str(ini).casefold(), 'ReShade preset cannot be its configuration file.')
            raw = path.read_bytes()
            plan.append((path, raw, retire_removed_effect_settings(sanitize_reshade_preset(raw))))
        plan = [(p, a, b) for p, a, b in plan if a != b]
        if not plan: return dict(changed=0, paths=[])
        history = core._plain(self.state_dir / 'reshade-settings-history', self.state_dir)
        history.mkdir(exist_ok=True)
        journal = history / (uuid.uuid4().hex + '-shortcuts.json')
        doc = dict(schema='texturequest-reshade-shortcuts-v1', phase='prepared', files=[])
        with ExitStack() as stack:
            retained = {}
            for path, raw, after in plan:
                stack.enter_context(DirectoryCustody(path.parent))
                held = stack.enter_context(FileCustody(path))
                retained[str(path)] = held
                held.checked(dict(name=path.name, bytes=len(raw), sha256=_sha(raw)))
                backup = history / (_sha(raw) + '.ini')
                if not backup.exists():
                    with backup.open('xb') as stream:
                        stream.write(raw); stream.flush(); os.fsync(stream.fileno())
                need(backup.read_bytes() == raw, 'ReShade settings backup changed; originals preserved.')
                doc['files'].append(dict(path=str(path), before_sha256=_sha(raw), after_sha256=_sha(after), backup=str(backup)))
            core._atomic(journal, doc)
            for path, raw, after in plan:
                ensure_closed()
                retained[str(path)].checked(dict(name=path.name, bytes=len(raw), sha256=_sha(raw)))
                temp = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
                try:
                    with temp.open('xb') as stream:
                        stream.write(after); stream.flush(); os.fsync(stream.fileno())
                    with DirectoryCustody(path.parent) as directories, FileCustody(temp) as output:
                        output.checked(dict(name=temp.name, bytes=len(after), sha256=_sha(after)))
                        # Keep the exact original handle through its move. A
                        # concurrent writer cannot be overwritten by our commit.
                        original = retained[str(path)]
                        saved = path.with_name('.' + path.name + '.tq-old-' + uuid.uuid4().hex)
                        row = next(x for x in doc['files'] if x['path'] == str(path))
                        row['original_stash'] = str(saved); core._atomic(journal, doc)
                        original.rename_new(saved, directories)
                        try:
                            output.rename_new(path, directories)
                        except Exception:
                            if not path.exists(): original.rename_new(path, directories)
                            raise
                        original.remove()
                finally: temp.unlink(missing_ok=True)
                next(x for x in doc['files'] if x['path'] == str(path))['committed'] = True
                core._atomic(journal, doc)
        doc['phase'] = 'complete'; core._atomic(journal, doc)
        return dict(changed=len(plan), paths=[str(p) for p, _, _ in plan], receipt=str(journal))

    def repair_shortcuts(self, game, progress=None):
        game = core._plain(core.validate_game(game).parent)
        ensure_closed()
        with core._lock(self.state_dir / 'operation.lock'), core._lock(self.state_dir / 'sideload/helper-owner.lock'):
            result = self._repair_shortcuts(game)
        result['message'] = 'ReShade shortcuts reset. Home opens ReShade; your TextureQuest shortcut is unchanged. Restart EQ2.'
        if progress: progress(result['message'])
        return result

    def _effects_shader(self, runtime):
        """Keep one immutable owned FX cache, shared like the local texture cache."""
        effects = runtime.get('effects')
        need(effects is not None, 'The optional effects runtime is not included.')
        pin = next(row for row in runtime['files'] if row['name'] == effects['shader'])
        source = self.runtime_dir / effects['shader']
        _pin(source, pin)
        raw = source.read_bytes()
        need(len(raw) == pin['bytes'] and _sha(raw) == pin['sha256'], 'Bundled effects shader changed while reading.')
        folder = core._plain(self.state_dir / 'effects' / pin['sha256'], self.state_dir)
        folder.mkdir(parents=True, exist_ok=True)
        path = core._plain(folder / 'TQEffects.fx', self.state_dir)
        if not path.exists():
            with path.open('xb') as stream:
                stream.write(raw); stream.flush(); os.fsync(stream.fileno())
        need(path.read_bytes() == raw, 'Saved effects shader changed; it was preserved.')
        # A historical private preview predated settings leases. Recognize just
        # its one exact cache artifact; never infer ownership from a hash name.
        from texturequest_effects import LEGACY_SHADER_SHA256
        legacy = core._plain(self.state_dir / 'effects' / LEGACY_SHADER_SHA256, self.state_dir)
        retired = None
        if legacy != folder and legacy.is_dir():
            entries = list(legacy.iterdir())
            if len(entries) == 1 and entries[0].name == 'TQEffects.fx':
                old = core._plain(entries[0], self.state_dir)
                if old.is_file() and old.stat().st_size <= 4 * 1024**2 and _pin(old)['sha256'] == LEGACY_SHADER_SHA256:
                    retired = legacy
        return folder, retired

    def _restore_effect_settings(self, game, record, key='effects_settings'):
        lease = record.get(key)
        if not lease: return ()
        from texturequest_effects import restore_effects_config
        conflicts = []
        need(lease.get('schema') == 'texturequest-effects-lease-v1' and
             core._plain(lease.get('path', ''), game) == game / 'ReShade.ini', 'Effects settings ownership changed.')
        if not (game / 'ReShade.ini').exists(): return ('ReShade.ini missing',)
        def restore(raw):
            after, changed = restore_effects_config(raw, lease['journal'])
            conflicts.extend(changed)
            return after
        try:
            self._repair_shortcuts(game, restore, reset_shortcuts=False)
        except (ValueError, UnicodeError) as error:
            conflicts.append(str(error))
        return tuple(conflicts)

    def _addon_ini(self, game, map_path, components=None, *, dlss_backend='feeder', world_graphics=False):
        selection = component_selection(components)
        need(dlss_backend in ('feeder', 'aio'), 'Unknown DLSS backend.')
        lines = ['[TextureQuest]', 'Map=' + str(map_path), 'Downloads=' + str(self.state_dir / 'dds' / 'objects'),
                 'Shortcut=' + str(self.base / 'shortcut.txt'), 'GamePaks=' + str(game / 'paks'),
                 'Effects=' + ('1' if selection['effects'] and not world_graphics else '0'),
                 'WorldGraphics=' + ('1' if world_graphics else '0'),
                 'DLSS=' + ('1' if selection['dlss'] else '0'), 'DLSSBackend=' + dlss_backend, '']
        return '\r\n'.join(lines).encode('utf-8')

    def _restore_dlss_consumer_settings(self, game, record, *, preserve_controls=False, key='dlss_consumer_settings'):
        # Replacement transfers all current seeded choices; true Uninstall
        # retains the normal conditional key restore.
        if preserve_controls and key == 'graphics_defaults_settings': return ()
        lease = record.get(key)
        if not lease: return ()
        from texturequest_dlss import restore_dlss_config
        need(lease.get('schema') == ('texturequest-graphics-defaults-lease-v1' if key == 'graphics_defaults_settings' else 'texturequest-dlss-lease-v1') and
             core._plain(lease.get('path', ''), game) == game / 'ReShade.ini', 'DLSS settings ownership changed.')
        if not (game / 'ReShade.ini').exists(): return ('ReShade.ini missing',)
        conflicts = []
        def restore(raw):
            after, changed = restore_dlss_config(raw, lease['journal'], preserve_controls=preserve_controls)
            conflicts.extend(changed)
            return after
        try:
            self._repair_shortcuts(game, restore, reset_shortcuts=False)
        except (ValueError, UnicodeError) as error:
            conflicts.append(str(error))
        return tuple(conflicts)

    def status(self, game):
        runtime = self._runtime()
        dlss_available, dlss_message = False, 'Experimental DLSS is not included.'
        if runtime and runtime.get('dlss') is not None:
            try:
                self._dlss_profile(runtime)
                dlss_available, dlss_message = True, 'Experimental DLSS profile verified. Private testing only.'
            except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
                dlss_message = 'Experimental DLSS unavailable: ' + str(error)
        renderer_available, renderer_message = False, 'World renderer is not included.'
        if runtime and runtime.get('renderer') is not None:
            try:
                self._renderer_profile(runtime)
                renderer_available, renderer_message = True, 'World renderer verified. Private testing only.'
            except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
                renderer_message = 'World renderer unavailable: ' + str(error)
        result = dict(installed=False, running=False, active_sessions=0, available=runtime is not None,
                      dual_load=False, shortcut=normalize(), message='Graphics hook runtime is not included.',
                      components=component_selection(), effects_available=bool(renderer_available or (runtime and runtime.get('effects'))),
                      renderer_available=renderer_available, renderer_message=renderer_message,
                      dlss_available=dlss_available, dlss_installed=False, dlss_message=dlss_message,
                      dlss_backend=_dlss_contract(runtime['dlss'])['backend'] if dlss_available else None,
                      installed_app_version=None, installed_newer_app=False, refresh_required=False)
        if not str(game).strip():
            return result
        record = self._record(game)
        if record and record['phase'] != 'uninstalled':
            stamp = record.get('app_version')
            if 'app_version' in record:
                version_number(stamp)
            current_version = version_number(APP_VERSION)
            newer = stamp is not None and version_number(stamp) > current_version
            refresh = not newer and (stamp is None or version_number(stamp) < current_version)
            binding = normalize(record.get('shortcut'))
            missing = [Path(x['path']).name for x in record['files'] if not Path(x['path']).is_file()]
            refresh = refresh or (not newer and (record['phase'] != 'installed' or bool(missing)))
            refresh = refresh or bool(not newer and dlss_available and
                component_selection(record.get('components'))['dlss'] and record.get('dlss_profile') != runtime['dlss']['id'])
            refresh = refresh or bool(not newer and renderer_available and
                component_selection(record.get('components'))['effects'] and
                (record.get('renderer_profile') != runtime['renderer']['id'] or
                 record.get('renderer_descriptor_sha256') != _sha(catalog.canonical(runtime['renderer']))))
            message = ('Interrupted graphics installation. Close EQ2 and Uninstall before retrying.' if record['phase'] != 'installed' else
                       'Hook file missing (' + ', '.join(missing) + '). Close EQ2, then Uninstall and Install to repair.' if missing else
                       'A newer launcher installed this hook. Update TextureQuest before reinstalling it.' if newer else
                       'This launcher needs to refresh your graphics hook and ReShade settings. Close EQ2, then Finish upgrade.' if refresh else
                       'Old PAK-reading hook. Close EQ2, then Uninstall and Install to use DDS files.' if record.get('payload') != 'loose-dds' else
                       'Graphics hook installed. ' + label(binding) + ' switches Original/Upscaled.')
            return dict(result, installed=True, active_sessions=self._games_open(game), available=True, shortcut=binding, message=message,
                        components=component_selection(record.get('components')), installation_revision=record['record_sha256'],
                        dlss_installed=bool(record['phase'] == 'installed' and not missing and
                                            component_selection(record.get('components'))['dlss']),
                        dlss_backend=record.get('dlss_backend', 'feeder') if component_selection(record.get('components'))['dlss'] else None,
                        installed_app_version=stamp, installed_newer_app=newer, refresh_required=refresh)
        if self._proxy_170(game):
            return dict(result, installed=True, active_sessions=self._games_open(game), available=True,
                        refresh_required=True,
                        message='TextureQuest 1.7.0 graphics hook. Close EQ2 and Uninstall here, then Install the add-on.')
        legacy = self._legacy(game)
        if legacy:
            sessions = legacy.get('sessions', [])
            count = sum(bool(_alive(x, x['exe'])) for x in sessions)
            watcher = legacy.get('watcher')
            running = bool(watcher and _alive(watcher, Path(legacy['runtime_dir']) / 'TextureQuestGraphics.exe'))
            if legacy['phase'] != 'uninstalled' or running or count:
                return dict(result, installed=legacy['phase'] != 'uninstalled', running=running, active_sessions=count,
                            available=True, refresh_required=True,
                            message='Old injected graphics hook. Close EQ2 and Uninstall here, then Install the add-on.')
        if runtime:
            result['message'] = 'Upscaled textures through a ReShade add-on. ' + label() + ' switches Original/Upscaled. Close EQ2 to install.'
        return result

    def refresh_needed(self, game):
        """Owned hooks are refreshed once per newer launcher, even for equal DLLs."""
        return self.status(game)['refresh_required']

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

    def _activate_prepared(self, game, prepared):
        """Caller holds operation/helper locks. Old maps and DDS objects remain usable."""
        DDSStore(self.state_dir).verify_for_install(prepared)
        ensure_closed()
        need(str(core._plain(prepared['game'])).casefold() == str(core._plain(game)).casefold(),
             'These textures were matched to a different game folder. Verify the download again.')
        raw = core._plain(prepared['map']['path'], self.state_dir).read_bytes()
        self.base.mkdir(parents=True, exist_ok=True)
        temporary = self.base / ('map-' + uuid.uuid4().hex + '.tmp')
        try:
            with temporary.open('xb') as stream:
                stream.write(raw); stream.flush(); os.fsync(stream.fileno())
            os.replace(temporary, self.base / 'texturequest-map.bin')
        finally:
            temporary.unlink(missing_ok=True)
        return self.base / 'texturequest-map.bin'

    def activate_prepared(self, game, prepared):
        """An installed DDS add-on picks up a verified update on its next launch."""
        record = self._record(game)
        if not record or record['phase'] != 'installed' or record.get('payload') != 'loose-dds':
            return
        ensure_closed()
        with core._lock(self.state_dir / 'operation.lock'), core._lock(self.state_dir / 'sideload/helper-owner.lock'):
            ensure_closed()
            self._activate_prepared(game, prepared)

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

    def install(self, game, prepared, *, dual_load=False, progress=None, shortcut=None, components=None):
        need(dual_load is False, 'Dual-load is not supported.')
        selection = component_selection(components)
        report = progress or (lambda _: None)
        binding = normalize(shortcut)
        game = core._plain(core.validate_game(game).parent)
        runtime = self._runtime()
        need(runtime, 'Graphics runtime missing.')
        need(not selection['effects'] or runtime.get('renderer') is not None or runtime.get('effects'),
             'The optional effects runtime is not included.')
        ensure_closed()
        with core._lock(self.state_dir / 'operation.lock'), core._lock(self.state_dir / 'sideload/helper-owner.lock'):
            self._assert_shortcuts_settled(game)
            self._exclusive_mode(game)
            ensure_closed()
            dlss_profile = self._preflight_components(game, selection, runtime)
            renderer_profile = self._renderer_profile(runtime) if selection['effects'] and runtime.get('renderer') is not None else None
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
            report('Checking saved texture preparation and texture map.')
            map_path = self._activate_prepared(game, prepared)
            self._write_key(binding)
            effects_lease, dlss_lease, consumer_lease, defaults_lease = None, None, None, None
            shader_folder, retired_shader = self._effects_shader(runtime) if selection['effects'] and renderer_profile is None else (None, None)
            dlss_folder = self._dlss_shaders(dlss_profile) if dlss_profile is not None else None
            aio_defaults = dlss_profile is not None and _dlss_contract(dlss_profile).get('settings') == 'aio2'
            defaults_profile = 'graphics-' + ('world-' if renderer_profile is not None else '') + ('aio-' if aio_defaults else '') + 'defaults'
            if defaults_profile == 'graphics-defaults': defaults_profile = 'graphics-core-defaults'
            def configure_effects(raw):
                nonlocal effects_lease, dlss_lease, consumer_lease, defaults_lease
                from texturequest_effects import prepare_effects_config
                for folder, previous, key in ((shader_folder, retired_shader, 'effects'), (dlss_folder, None, 'dlss')):
                    if folder is None: continue
                    raw, journal = prepare_effects_config(raw, str(folder),
                        retired_shader_directory=str(previous) if previous is not None else None)
                    lease = dict(schema='texturequest-effects-lease-v1', path=str(game / 'ReShade.ini'), journal=journal)
                    old_lease = effects_lease if key == 'effects' else dlss_lease
                    need(old_lease is None or old_lease == lease, 'ReShade settings changed before installation; settings preserved.')
                    if key == 'effects': effects_lease = lease
                    else: dlss_lease = lease
                if dlss_folder is not None:
                    from texturequest_dlss import prepare_dlss_config
                    contract = _dlss_contract(dlss_profile)
                    raw, journal = prepare_dlss_config(raw, profile=contract.get('settings', contract['backend']))
                    lease = dict(schema='texturequest-dlss-lease-v1', path=str(game / 'ReShade.ini'), journal=journal)
                    need(consumer_lease is None or consumer_lease == lease, 'DLSS settings changed before installation; settings preserved.')
                    consumer_lease = lease
                from texturequest_dlss import prepare_dlss_config
                raw, journal = prepare_dlss_config(raw, profile=defaults_profile)
                lease = dict(schema='texturequest-graphics-defaults-lease-v1', path=str(game / 'ReShade.ini'), journal=journal)
                need(defaults_lease is None or defaults_lease == lease, 'Graphics defaults changed before installation; settings preserved.')
                defaults_lease = lease
                return raw
            if (game / 'ReShade.ini').exists():
                configure_effects(quiet_reshade((game / 'ReShade.ini').read_bytes()))
            writes, owned = [], False
            if all(kind == 'none' for kind in kinds.values()):
                owned = True
                writes.append((game / 'dxgi.dll', (self.runtime_dir / runtime['reshade']).read_bytes()))
            if not (game / 'ReShade.ini').exists():
                writes.append((game / 'ReShade.ini', configure_effects(DEFAULT_RESHADE_INI)))
            folder = self._addon_folder(game)
            writes.append((folder / 'TextureQuest.addon64', (self.runtime_dir / runtime['addon']).read_bytes()))
            dlss_backend = _dlss_contract(dlss_profile)['backend'] if dlss_profile is not None else 'feeder'
            writes.append((folder / 'TextureQuest.ini', self._addon_ini(game, map_path, selection, dlss_backend=dlss_backend, world_graphics=renderer_profile is not None)))
            if dlss_profile is not None:
                for row in dlss_profile['files']:
                    if row['role'] not in _dlss_contract(dlss_profile)['modules']: continue
                    source = core._plain(self.runtime_dir / row['name'], self.runtime_dir)
                    _pin(source, row)
                    raw = source.read_bytes()
                    need(len(raw) == row['bytes'] and _sha(raw) == row['sha256'], 'DLSS module changed while reading.')
                    writes.append((game / source.name, raw))
            if renderer_profile is not None:
                row = renderer_profile['files'][0]
                source = core._plain(self.runtime_dir / row['name'], self.runtime_dir)
                _pin(source, row)
                raw = source.read_bytes()
                need(len(raw) == row['bytes'] and _sha(raw) == row['sha256'], 'World renderer changed while reading.')
                writes.append((folder / RENDERER_MODULE, raw))
            for path, _ in writes:
                need(not path.exists(), path.name + ' already exists. It was left unchanged.')
            files = [dict(path=str(p), bytes=len(d), sha256=_sha(d)) for p, d in writes]
            doc = dict(schema=RECORD_SCHEMA, phase='installing', id=uuid.uuid4().hex, game=str(game), files=files,
                       reshade_owned=owned, shortcut=binding, map=str(map_path), payload='loose-dds',
                       components=selection, effects_settings=effects_lease, dlss_settings=dlss_lease,
                       dlss_consumer_settings=consumer_lease, graphics_defaults_settings=defaults_lease,
                       dlss_profile=dlss_profile['id'] if dlss_profile is not None else None,
                       dlss_backend=dlss_backend if dlss_profile is not None else None,
                       renderer_profile=renderer_profile['id'] if renderer_profile is not None else None,
                       renderer_descriptor_sha256=_sha(catalog.canonical(renderer_profile)) if renderer_profile is not None else None,
                       app_version=APP_VERSION,
                       runtime_descriptor_sha256=_sha((self.runtime_dir / 'RUNTIME.json').read_bytes()))
            self._save(game, doc)
            # Persist the optional settings lease before changing an existing
            # ReShade configuration, including interrupted installations.
            self._repair_shortcuts(game, configure_effects)
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
        # Native settings use their own normal locks and exact full backups.
        # Current effects supplement native rendering. No unverified disable
        # command is shipped; future reviewed overrides enter only here.
        settings = GameSettings(self.state_dir).apply(game, {}, features=(
            'ambient_occlusion', 'contact_shadows'
        ) if selection['effects'] and renderer_profile is None else ('world_graphics',) if renderer_profile is not None else ())
        report('Graphics hook installed. ' + label(binding) + ' switches quality.')
        return dict(self.status(game), native_settings=settings)

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

    def uninstall(self, game, progress=None, *, expected_record=..., preserve_dlss_controls=False):
        game = core._plain(core.validate_game(game).parent)
        ensure_closed()
        kept = []
        with core._lock(self.state_dir / 'operation.lock'), core._lock(self.state_dir / 'sideload/helper-owner.lock'):
            doc = self._record(game)
            if expected_record is not ...:
                need(doc == expected_record, 'Graphics installation record changed. The installed hook was kept.')
            if not doc or doc['phase'] == 'uninstalled':
                if self._proxy_170(game):
                    kept = self._uninstall_proxy_170(game)
                    message = 'Graphics hook removed.' + (' Changed files kept: ' + ', '.join(kept) + '.' if kept else '')
                    if progress:
                        progress(message)
                    return dict(self.status(game), message=message, kept=kept)
                legacy = self._legacy(game)
                need(legacy and legacy['phase'] != 'uninstalled', 'No installed graphics hook.')
                self._uninstall_legacy(game, legacy)
                if progress:
                    progress('Old graphics hook removed.')
                return self.status(game)
            # The DLSS path was added last; reverse leases so shared loading
            # keys and preexisting user paths restore without false conflicts.
            settings_conflicts = (self._restore_dlss_consumer_settings(game, doc, preserve_controls=preserve_dlss_controls, key='graphics_defaults_settings') +
                                  self._restore_dlss_consumer_settings(game, doc, preserve_controls=preserve_dlss_controls) +
                                  self._restore_effect_settings(game, doc, 'dlss_settings') +
                                  self._restore_effect_settings(game, doc))
            for row in doc['files']:
                p = core._plain(row['path'])
                if not p.exists():
                    continue
                # A replacement hands current controls to the new preparation,
                # including an originally created INI or an empty old lease.
                # The real Uninstall still follows its original ownership rules.
                if preserve_dlss_controls and p == game / 'ReShade.ini':
                    kept.append(p.name)
                    continue
                if p.name.casefold() in PROXY_NAMES and not doc['reshade_owned']:
                    continue
                if _sha(p.read_bytes()) != row['sha256']:
                    kept.append(p.name)
                    continue
                ensure_closed()
                p.unlink()
            # Do not declare completion before native INI restoration. If that
            # separate transaction refuses, normal Uninstall can finish it.
            doc['phase'] = 'uninstalling'
            doc['kept'] = kept
            doc['settings_conflicts'] = settings_conflicts
            self._save(game, doc)
            doc = self._record(game)  # canonical JSON lists for the later CAS
        native_settings = GameSettings(self.state_dir).restore(game)
        with core._lock(self.state_dir / 'operation.lock'), core._lock(self.state_dir / 'sideload/helper-owner.lock'):
            ensure_closed()
            need(self._record(game) == doc, 'Graphics record changed during settings restore; preserve its journals.')
            doc['phase'] = 'uninstalled'
            self._save(game, doc)
        message = 'Graphics hook removed.' + (' Changed files kept: ' + ', '.join(kept) + '.' if kept else '')
        if settings_conflicts: message += ' Changed ReShade settings were kept: ' + ', '.join(settings_conflicts) + '.'
        if native_settings.get('conflicts'): message += ' Later game settings were kept; see the native-settings journal.'
        if progress:
            progress(message)
        return dict(self.status(game), message=message, kept=kept, settings_conflicts=settings_conflicts,
                    native_settings=native_settings)

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
