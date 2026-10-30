"""Third, exclusive installation mode: packaged graphics runtime adapter.

An absent/unverified full-pack runtime is unavailable, never a successful
installation. All process injection lives in the separately built controller.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess

from texture_swapper import core
from texturequest_processes import ensure_closed
from texturequest_sideload import LoaderManager, _alive, _pin, _json, _live_status_bytes
from texturequest_shortcut import normalize as shortcut_spec,label as shortcut_label

CAPABILITIES = {'full_pack', 'on_demand', 'dual_load', 'foreground_toggle', 'bounded_cache','custom_shortcut'}


class GraphicsManager:
    def __init__(self, state_dir, *, runtime_dir=None):
        self.state_dir = core._plain(state_dir)
        self.base = self.state_dir / 'graphics'
        self.runtime_dir = Path(runtime_dir) if runtime_dir else Path(__file__).resolve().parent / 'graphics-runtime'
        self.runtime_cache = None
        location = self.state_dir/'graphics-cache-location.json'
        self.cache_base = self.base
        if location.exists():
            value = core._load(location)
            if value.get('schema') != 'texturequest-graphics-cache-location-v1':
                raise ValueError('Unknown graphics cache location; preserve it for recovery.')
            self.cache_base = core._plain(value['cache_root'])

    def _runtime(self):
        if self.runtime_cache:
            saved, stamps = self.runtime_cache
            if all((p.stat().st_size,p.stat().st_mtime_ns,p.stat().st_ctime_ns,p.stat().st_ino)==s for p,s in stamps):
                return saved
        descriptor = self.runtime_dir / 'RUNTIME.json'
        if not descriptor.exists():
            return None
        doc = _json(descriptor)
        if (doc.get('schema') != 'texturequest-graphics-runtime-v1' or
                set(doc.get('capabilities', ())) != CAPABILITIES or doc.get('full_pack_prepared') is not True):
            raise ValueError('The graphics runtime has not passed its full-pack preparation checks.')
        rows = doc.get('files')
        if type(rows) is not list or not 4 <= len(rows) <= 64:
            raise ValueError('Invalid graphics runtime file roster.')
        names = set()
        for row in rows:
            name = row['name']
            if (type(name) is not str or Path(name).name != name or name.casefold() in names):
                raise ValueError('Aliased or escaped graphics runtime file.')
            names.add(name.casefold())
            _pin(self.runtime_dir/name, dict(bytes=row['bytes'], sha256=row['sha256']))
        if doc.get('controller') != 'TextureQuestGraphics.exe' or 'texturequestgraphics.exe' not in names:
            raise ValueError('Graphics controller is missing from its file roster.')
        paths = [descriptor, *(self.runtime_dir/r['name'] for r in rows)]
        self.runtime_cache = doc, [(p,(p.stat().st_size,p.stat().st_mtime_ns,p.stat().st_ctime_ns,p.stat().st_ino)) for p in paths]
        return doc

    def _key(self, game):
        return hashlib.sha256(str(core._plain(game)).casefold().encode('utf-8')).hexdigest()[:24]

    def _record_path(self, game):
        return self.base/'registrations'/(self._key(game)+'.json')

    def _record(self, game):
        path = self._record_path(game)
        if not path.exists():
            return None
        raw=_live_status_bytes(path)
        if raw is None:return None
        doc=core._json(raw)
        if doc.get('record_sha256')!=core._digest({k:v for k,v in doc.items() if k!='record_sha256'}):
            raise ValueError('Graphics registration checksum changed; preserve it for recovery.')
        if (doc.get('schema') != 'texturequest-graphics-installation-v1' or
                str(core._plain(doc['game'])).casefold() != str(core._plain(game)).casefold() or
                doc.get('phase') not in {'installed', 'uninstalled', 'preparing', 'failed'} or
                not re.fullmatch('[a-f0-9]{32}', doc.get('installation_id', '')) or type(doc.get('dual_load')) is not bool):
            raise ValueError('Unknown graphics installation record; preserve it for recovery.')
        return doc

    def status(self, game):
        runtime = self._runtime()
        value = dict(installed=False, running=False, active_sessions=0, available=runtime is not None,
                     message='Graphics hook runtime is being prepared for the full pack; this build cannot install the shield-only test.',
                     dual_load=False,shortcut=shortcut_spec())
        if not str(game).strip():
            return value
        record = self._record(game)
        if record is None:
            if runtime:
                value['message'] = 'On-demand graphics replacements; '+shortcut_label()+' switches quality.'
            return value
        sessions = record.get('sessions', [])
        if type(sessions) is not list or len(sessions) > 128:
            raise ValueError('Invalid retained graphics sessions.')
        for session in sessions:
            if type(session.get('pid')) is not int or type(session.get('birth100ns')) is not int or not session.get('exe'):
                raise ValueError('Invalid graphics session identity.')
        count = sum(bool(_alive(session, session['exe'])) for session in sessions)
        watcher = record.get('watcher')
        running = bool(watcher and _alive(watcher, str(Path(record['runtime_dir'])/'TextureQuestGraphics.exe')))
        installed = record['phase'] in {'installed', 'preparing'}
        shortcut=shortcut_spec(record.get('shortcut'))
        message = 'Graphics hook installed. '+shortcut_label(shortcut)+' switches Original / Upscaled while EQ2 is focused.'
        if record['phase']=='preparing':
            message = 'Preparing the full-pack graphics cache. Game files stay unchanged.'
        elif installed and not running:
            message = 'Graphics watcher is not running. Uninstall and reinstall Graphics hook before the next game launch.'
        if record.get('watch_state') == 'refresh_required':
            message = 'The game patched. Close EQ2, then reinstall Graphics hook to refresh its private cache; game files stay unchanged.'
        elif record.get('watch_state') == 'restart_needed':
            message = 'Graphics hook missed startup. Close EQ2 and launch it again normally.'
        if record.get('shortcut_state',{}).get('error'):
            message=record['shortcut_state']['error']
        return dict(value, installed=installed, running=running, active_sessions=count,
                    available=runtime is not None or installed, dual_load=record['dual_load'],shortcut=shortcut,
                    message=(message if installed else
                             'Graphics hook uninstalled for future launches; active clients keep it until they exit.'))

    def cache_in_use(self):
        records = self.base/'registrations'
        if not records.exists():
            return False
        for path in core._plain(records, self.base).iterdir():
            if not path.is_file() or not re.fullmatch(r'[a-f0-9]{24}\.json', path.name):
                raise ValueError('Unknown graphics registration; cached files are preserved.')
            record = _json(path)
            status = self.status(record['game'])
            if status['installed'] or status['running'] or status['active_sessions']:
                return True
        return False

    def installed_game(self):
        records = self.base/'registrations'
        if not records.exists():
            return None
        games = []
        for path in core._plain(records, self.base).iterdir():
            record = _json(path)
            if self.status(record['game'])['installed']:
                games.append(record['game'])
        if len(games) > 1:
            raise ValueError('Multiple graphics installations require an explicit game folder.')
        return games[0] if games else None

    def _action(self, action, game, prepared=None, dual_load=False, progress=None, shortcut=None):
        runtime = self._runtime()
        if runtime is None:
            raise RuntimeError('The complete graphics hook runtime is not included in this build.')
        if type(dual_load) is not bool:
            raise ValueError('Dual-load must be a Boolean.')
        game = core._plain(core.validate_game(game).parent)
        if self.cache_base.is_relative_to(game) or game.is_relative_to(self.cache_base):
            raise ValueError('Graphics cache must be outside the game folder.')
        if self.state_dir.is_relative_to(game) or game.is_relative_to(self.state_dir):
            raise ValueError('Graphics data must be outside the game folder.')
        if action == 'install':
            ensure_closed()
            actual = core._load(self.state_dir/'direct-prepared.json')
            if actual != prepared or actual.get('schema') != 'texturequest-direct-prepared-v1':
                raise ValueError('Prepare and verify the selected full texture pack first.')
        argv = [str(self.runtime_dir/'TextureQuestGraphics.exe'), '--'+action, '--state', str(self.state_dir),
                '--game', str(game), '--dual-load', '1' if dual_load else '0']
        binding=shortcut_spec(shortcut)
        argv+=['--hotkey-vk',str(binding['vk']),'--hotkey-modifiers',str(binding['modifiers'])]
        if progress:
            progress('Preparing full-pack graphics replacements. Game files stay unchanged.' if action == 'install' else
                     'Saving the texture shortcut…' if action=='shortcut' else 'Uninstalling the graphics watcher for future launches.')
        # This is an explicit operation, not a discovery probe or a status poll.
        self.base.mkdir(parents=True, exist_ok=True)
        # The child owns the shared operation locks. Holding them here would
        # deadlock its independently verified install/uninstall transaction.
        process = subprocess.Popen(argv + ['--runtime', str(self.runtime_dir)], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace',
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        tail=[]
        with process.stdout:
            for line in process.stdout:
                text=line.rstrip()[:1024]
                tail=(tail+[text])[-8:]
                if progress and text:
                    progress(text)
        if process.wait():
            raise RuntimeError('\n'.join(tail)[-4096:] or 'Graphics operation failed; its history was preserved.')
        status = self.status(game)
        if status['installed'] != (action in {'install','shortcut'}):
            raise RuntimeError('Graphics operation did not confirm the requested installed state.')
        return status

    def install(self, game, prepared, *, dual_load=False, progress=None, shortcut=None):
        return self._action('install', game, prepared, dual_load, progress,shortcut)

    def set_shortcut(self,game,shortcut,progress=None):
        return self._action('shortcut',game,progress=progress,shortcut=shortcut)

    def uninstall(self, game, progress=None):
        return self._action('uninstall', game, progress=progress)
