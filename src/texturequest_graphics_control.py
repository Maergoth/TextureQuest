"""Explicit graphics installation and ordinary-launch watcher.

Only the separate native early-attach helper touches the selected game process.
This supervisor never suspends, kills, resumes or writes game files. No tasks or
services are registered: the user's owned HKCU startup entry runs the watcher.
"""
from __future__ import annotations
from contextlib import contextmanager
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import uuid
import psutil
from texture_swapper import core
from texturequest_graphics import GraphicsManager
from texturequest_sideload import (LoaderManager, _prepare, _json, _pin, _alive,
    _process_identity, _same_path, _read_run, _write_run, _remove_run, _signal_stop)
from texturequest_processes import ensure_closed
import texturequest_graphics_catalog as catalog
import texturequest_graphics_vpk as vpk
from texturequest_shortcut import ShortcutPump,WindowsKeys,normalize as shortcut_spec,label as shortcut_label


def require(value, message):
    if not value:
        raise ValueError(message)


def exclusive_json(path, value):
    path = core._plain(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as stream:
        stream.write(catalog.canonical(value) + b'\n')
        stream.flush()
        os.fsync(stream.fileno())


def emit(text):
    # _prepare redirects helper stdout into this callback. Writing to that
    # redirected stream would recursively feed the progress callback itself.
    sys.__stdout__.write(str(text)+'\n')
    sys.__stdout__.flush()


def command(exe, *args):
    return subprocess.list2cmdline([str(exe), *map(str, args)])


def source_stamps(ready_path):
    ready = _json(ready_path)
    return [dict(path=p['source']['path'], stat=list(catalog.stat5(Path(p['source']['path']).stat())))
            for p in ready['providers']]


def memory_manager(manager, *, component=False):
    # Native named-file custody deliberately binds the portable component to
    # its download data root. Only the graphics comparison cache is relocatable.
    memory = LoaderManager(manager.state_dir)
    # The supervisor is a separate frozen executable. Its verified sibling
    # resources live with the app, not in the one-file extraction directory.
    memory.descriptor = manager.runtime_dir.parent/'texturequest-sideload-runtime.json'
    return memory


def build_epoch(manager, game, runtime, dual_load, epoch, report=emit):
    """Prepare named upscales and their stock comparison into a NEW epoch."""
    require(not epoch.exists(), 'Preserve the previous graphics cache epoch.')
    epoch.mkdir(parents=True)
    memory = memory_manager(manager, component=True)
    admitted = memory._runtime()
    require(admitted is not None and admitted.get('portable_pending'), 'Verified portable name loader is required.')
    component = memory._materialize(admitted['bundle'], game)
    prior = memory._record(game)
    if (prior and prior.get('phase')=='uninstalled' and
            prior.get('ready',{}).get('ready_json') and
            prior.get('runtime',{}).get('files')==component['files']):
        # _prepare performs the existing complete membership/stat choice,
        # then full verify_ready. Failure never falls back to unchecked reuse.
        component['prepared_ready'] = prior['ready']['ready_json']
    named_epoch = Path(component['preparation_root'])/uuid.uuid4().hex
    named_epoch.mkdir(parents=True)
    prepared = _prepare(component, game, manager.state_dir/'direct-prepared.json', named_epoch/'pack', report)
    ready_path = Path(prepared['ready_json']['path'])
    comparison = catalog.prepare(ready_path, epoch/'comparison', vpk,
        lambda text, *_: report(text), direction='upscaled_canonical')
    require(comparison['rows'] == prepared['applied'] and not comparison['gaps'],
            'Full-pack comparison contains an unsupported format or missing row.')
    # Ambiguous Original fingerprints preserve the correctly named upscale.
    # They do not prevent full named Upscaled delivery and are never guessed.
    report(str(comparison['rows'])+' upscales; '+str(len(comparison['ambiguous']))+' ambiguous Original comparisons stay upscaled.')
    index = epoch/'comparison/catalog.bin'
    payload = epoch/'comparison/payloads.bin'
    raw_index = index.read_bytes()
    require(len(raw_index) == comparison['index']['bytes'] and
            hashlib.sha256(raw_index).hexdigest() == comparison['index']['sha256'], 'Comparison index changed.')
    encoded = str(payload).encode('utf-8')
    envelope = b'TQGXCFG1'+struct.pack('<IIII', 1, int(dual_load), len(encoded), len(raw_index))+encoded+raw_index
    config = epoch/'graphics-config.bin'
    with config.open('xb') as stream:
        stream.write(envelope)
    return dict(named=prepared, config=_pin(config), catalog=_pin(epoch/'comparison/CATALOG.json'),
                source_stamps=source_stamps(ready_path), rows=comparison['rows'],
                ambiguous_original=len(comparison['ambiguous']), component=component['files'])


def attach_request(record, identity, runtime, root):
    """Pure request construction except for fresh executable hashing.

    All module pins come from the verified runtime, not caller supplied hashes.
    The native helper reopens/holds and verifies them before any remote call.
    """
    game = Path(record['game'])
    require(_same_path(identity['exe'], game/'EverQuest2.exe'), 'Foreign process cannot become a graphics session.')
    nonce = uuid.uuid4().hex
    session = root/nonce
    named = record['epoch']['named']
    files = {r['name']: r for r in runtime['files']}
    role_names = dict(provider='ReShade64.dll', host='TextureQuestGraphicsHost.addon64',
                      bootstrap='TextureQuestGraphicsBootstrap.dll')
    result = dict(nonce=nonce, pid=str(identity['pid']), birth100ns=str(identity['birth100ns']))
    def path(key, value):
        result[key] = str(core._plain(value)).encode('utf-8').hex()
    path('target', identity['exe'])
    result['target_sha256'] = _pin(identity['exe'])['sha256']
    path('session', session)
    path('game', game)
    for role, name in role_names.items():
        row = files[name]
        path(role, Path(record['runtime_dir'])/name)
        result[role+'_sha256'] = row['sha256']
    memory = record['epoch']['component']['dll']
    for role, pin in [('memory', memory), ('manifest', named['manifest']), ('bindings', named['bindings']),
                      ('catalog', record['epoch']['config'])]:
        path(role, pin['path'])
        result[role+'_sha256'] = pin['sha256']
    path('package', Path(memory['path']).parent)
    body = 'TQGXREQ1\n'+''.join(k+'='+v+'\n' for k,v in result.items())
    require(len(body.encode('ascii')) <= 65535, 'Graphics request exceeds its bounded wire size.')
    return nonce, session, body.encode('ascii')


class StopEvent:
    def __init__(self, name):
        require(__import__('re').fullmatch(r'Local\\TQWatch-[a-f0-9]{32}', name), 'Invalid watcher event.')
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        self.k.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
        self.k.CreateEventW.restype = wintypes.HANDLE
        self.k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.k.WaitForSingleObject.restype = wintypes.DWORD
        self.k.CloseHandle.argtypes = [wintypes.HANDLE]
        self.k.CloseHandle.restype = wintypes.BOOL
        self.handle = self.k.CreateEventW(None, True, False, name)
        require(self.handle and ctypes.get_last_error() != 183, 'Watcher event already exists or could not be created.')
    def wait(self, milliseconds):
        value = self.k.WaitForSingleObject(self.handle, milliseconds)
        require(value in (0,258), 'Watcher wait failed.')
        return value == 0
    def close(self):
        require(self.k.CloseHandle(self.handle), 'Watcher event close failed.')


def game_processes(game):
    target = Path(game)/'EverQuest2.exe'
    result = []
    for p in psutil.process_iter(['pid','name']):
        if (p.info.get('name') or '').casefold() != 'everquest2.exe':
            continue
        identity = _process_identity(p.info['pid'])
        if identity and _same_path(identity['exe'], target):
            result.append(identity)
    return result


def save_record(manager, game, value):
    core._save(manager._record_path(game), value)


@contextmanager
def record_lock(manager):
    deadline=time.monotonic()+2
    while True:
        lock=core._lock(manager.base/'record.lock')
        try:lock.__enter__()
        except OSError:
            if time.monotonic()>=deadline:raise
            time.sleep(.01)
        else:break
    try:yield
    finally:lock.__exit__(None,None,None)


def publish_watcher_ready(manager,game,record,mine):
    record['watcher']=mine
    record['watch_state']='armed'
    home=Path(record['home'])
    exclusive_json(home/'watcher-history'/(str(mine['pid'])+'-'+str(mine['birth100ns'])+'.json'),mine)
    # Readiness is the sealed registration's PID/birth plus immutable history.
    # Never replace a fixed readiness FILE beneath a live retained game tree.
    save_record(manager,game,record)


def archive_closed_sessions(record):
    active=[]
    for session in record['sessions']:
        if _alive(session,session['exe']):active.append(session);continue
        path=Path(record['home'])/'session-history'/(str(session['pid'])+'-'+str(session['birth100ns'])+'.json')
        if path.exists():require(_json(path)==session,'Retained session history differs; preserve it.')
        else:exclusive_json(path,session)
    record['sessions']=active
    require(len(active)<128,'Too many simultaneous retained clients; no new attachment was attempted.')


def watch(manager, game, runtime):
    path = manager._record_path(game)
    initial = manager._record(game)
    require(initial and initial['phase'] == 'installed', 'Graphics installation is not armed.')
    event = StopEvent(initial['stop_event'])
    shortcut=ShortcutPump(WindowsKeys(),_alive)
    try:
        # A separate single-owner lock never blocks the normal install lock.
        with core._lock(manager.base/('watch-'+manager._key(game)+'.lock')):
            mine = _process_identity(os.getpid())
            require(mine and _same_path(mine['exe'], Path(initial['runtime_dir'])/'TextureQuestGraphics.exe'), 'Watcher image identity differs.')
            with record_lock(manager):
                record = manager._record(game)
                require(record['installation_id'] == initial['installation_id'] and record['phase'] == 'installed', 'Watcher installation changed before handshake.')
                publish_watcher_ready(manager,game,record,mine)
            seen = {(r['pid'],r['birth100ns']) for r in record['sessions']}
            while not event.wait(50):
                with record_lock(manager):
                    record = manager._record(game)
                if record['phase'] != 'installed' or record['installation_id'] != initial['installation_id']:
                    return
                key_state=shortcut.update(record)
                if key_state!=record.get('shortcut_state'):
                    with record_lock(manager):
                        fresh=manager._record(game)
                        if (fresh['installation_id']==record['installation_id'] and
                                shortcut_spec(fresh.get('shortcut'))==key_state['binding']):
                            fresh['shortcut_state']=key_state
                            save_record(manager,game,fresh)
                for identity in game_processes(game):
                    key = identity['pid'],identity['birth100ns']
                    if key in seen:
                        continue
                    seen.add(key)
                    with record_lock(manager):
                        record=manager._record(game)
                        archive_closed_sessions(record)
                        save_record(manager,game,record)
                    current100ns = time.time_ns()//100 + 116444736000000000
                    require(current100ns >= identity['birth100ns'], 'Game birth is in the future.')
                    if current100ns-identity['birth100ns'] > 20000000:
                        state = 'restart_needed'
                    elif source_stamps(record['epoch']['named']['ready_json']['path']) != record['epoch']['source_stamps']:
                        state = 'refresh_required'
                    else:
                        nonce, session, raw = attach_request(record, identity, runtime, Path(record['home'])/'sessions')
                        request = Path(record['home'])/'requests'/(nonce+'.txt')
                        request.parent.mkdir(parents=True,exist_ok=True)
                        session.parent.mkdir(parents=True,exist_ok=True)
                        with request.open('xb') as stream:
                            stream.write(raw)
                        helper = Path(record['runtime_dir'])/'TQGraphicsAttach.exe'
                        _pin(helper, next(r for r in runtime['files'] if r['name']==helper.name))
                        log = request.with_suffix('.stderr.txt')
                        with log.open('xb') as stderr:
                            process = subprocess.Popen([str(helper),'--attach',str(request)], stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW|subprocess.BELOW_NORMAL_PRIORITY_CLASS)
                        identity = dict(identity, coordinator=_process_identity(process.pid), session=str(session), request=str(request))
                        state = 'attaching'
                    with record_lock(manager):
                        fresh = manager._record(game)
                        require(fresh['installation_id']==record['installation_id'], 'Installation changed during an attach outcome.')
                        # Retain every game identity, including a failed/late attach;
                        # its possibly loaded DLLs prevent cache replacement.
                        fresh['sessions'].append(dict(identity, outcome=state))
                        require(len(fresh['sessions']) <= 128, 'Session history limit reached; preserve this installation.')
                        fresh['watch_state'] = state
                        save_record(manager,game,fresh)
    finally:
        shortcut.close()
        event.close()


def install(manager, game, runtime, dual, shortcut=None):
    ensure_closed()
    with core._lock(manager.state_dir/'operation.lock'),core._lock(manager.state_dir/'sideload/helper-owner.lock'):
        require(not memory_manager(manager).cache_in_use(), 'Uninstall Memory hook and close its sessions first.')
        old = manager._record(game)
        if old:
            status = manager.status(game)
            require(not status['installed'] and not status['running'] and not status['active_sessions'], 'Uninstall Graphics hook and close its sessions before reinstalling.')
            exclusive_json(Path(old['home'])/'record-history'/(uuid.uuid4().hex+'.json'),old)
        installation = uuid.uuid4().hex
        require(not manager.cache_base.is_relative_to(game) and not game.is_relative_to(manager.cache_base),'Cache must be outside game.')
        home = manager.cache_base/'installations'/installation
        home.mkdir(parents=True)
        record = dict(schema='texturequest-graphics-installation-v1', phase='preparing', installation_id=installation,
            game=str(game),dual_load=dual,runtime_dir=str(manager.runtime_dir),home=str(home),sessions=[],watcher=None,
            stop_event='Local\\TQWatch-'+installation,watch_state='preparing',shortcut=shortcut_spec(shortcut))
        save_record(manager,game,record)
        try:
            record['epoch'] = build_epoch(manager,game,runtime,dual,home/'epoch-001')
            ensure_closed()
            argv = [str(manager.runtime_dir/'TextureQuestGraphics.exe'),'--watch','--state',str(manager.state_dir),
                    '--game',str(game),'--runtime',str(manager.runtime_dir)]
            record['startup_name'] = 'TextureQuestGraphics-'+manager._key(game)
            record['startup_command'] = command(*argv)
            require(_read_run(record['startup_name']) is None, 'An unowned startup value is preserved.')
            _write_run(record['startup_name'],None,record['startup_command'])
            record['phase'] = 'installed'
            save_record(manager,game,record)
            with (home/'watcher.stdout.txt').open('xb') as stdout,(home/'watcher.stderr.txt').open('xb') as stderr:
                process = subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=stdout,stderr=stderr,
                    env=dict(os.environ,PYINSTALLER_RESET_ENVIRONMENT='1'),
                    creationflags=subprocess.CREATE_NO_WINDOW|subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            deadline=time.monotonic()+20
            ready=None
            while time.monotonic()<deadline:
                fresh=manager._record(game)
                if fresh.get('watcher') and _alive(fresh['watcher'],argv[0]):
                    ready=fresh['watcher'];break
                require(process.poll() is None,'Watcher exited before readiness; installation history retained.')
                time.sleep(.05)
            require(ready is not None,'Watcher readiness deadline; preserve installation history.')
            require(_alive(ready,argv[0]),'Watcher readiness PID/birth does not match a live controller.')
            emit('Graphics hook installed. Full pack ready; '+shortcut_label(record['shortcut'])+' switches quality.')
        except BaseException as error:
            # Do not replace a watcher's fresher session history on failure.
            with record_lock(manager):
                fresh=manager._record(game)
                if fresh and fresh['installation_id']==installation:
                    if fresh.get('startup_name'):
                        _remove_run(fresh['startup_name'],fresh['startup_command'])
                    fresh['phase']='failed';fresh['failure']=str(error)
                    save_record(manager,game,fresh)
            raise


def uninstall(manager, game):
    with core._lock(manager.state_dir/'operation.lock'),core._lock(manager.state_dir/'sideload/helper-owner.lock'):
        with record_lock(manager):
            record=manager._record(game)
            require(record is not None,'No owned graphics installation exists.')
            if record.get('startup_name'):
                _remove_run(record['startup_name'],record['startup_command'])
            if record.get('watcher') and _alive(record['watcher'],Path(record['runtime_dir'])/'TextureQuestGraphics.exe'):
                _signal_stop(record['stop_event'])
            record['phase']='uninstalled';record['watch_state']='stopped'
            save_record(manager,game,record)
        deadline=time.monotonic()+10
        while record.get('watcher') and _alive(record['watcher'],Path(record['runtime_dir'])/'TextureQuestGraphics.exe'):
            require(time.monotonic()<deadline,'Watcher stop not confirmed; preserve app and caches until it exits.')
            time.sleep(.05)
        emit('Graphics watcher uninstalled. Active games retain their existing session until they exit normally.')


def set_shortcut(manager,game,binding):
    binding=shortcut_spec(binding)
    with core._lock(manager.state_dir/'operation.lock'),record_lock(manager):
        record=manager._record(game)
        require(record and record['phase']=='installed','Install Graphics hook before saving a live shortcut.')
        exclusive_json(Path(record['home'])/'shortcut-history'/(uuid.uuid4().hex+'.json'),
            dict(before=record.get('shortcut'),after=binding,game=str(game)))
        record['shortcut']=binding
        record.pop('shortcut_state',None)
        save_record(manager,game,record)
    emit('Shortcut saved: '+shortcut_label(binding)+'. Active only while EQ2 is focused.')


def main():
    parser=argparse.ArgumentParser()
    action=parser.add_mutually_exclusive_group(required=True)
    for name in ('install','uninstall','watch','shortcut'):
        action.add_argument('--'+name,action='store_true')
    parser.add_argument('--state',required=True,type=Path)
    parser.add_argument('--game',required=True,type=Path)
    parser.add_argument('--runtime',required=True,type=Path)
    parser.add_argument('--dual-load',choices=('0','1'),default='0')
    parser.add_argument('--hotkey-vk',type=int,default=0x6a)
    parser.add_argument('--hotkey-modifiers',type=int,default=0)
    args=parser.parse_args()
    manager=GraphicsManager(args.state,runtime_dir=args.runtime)
    runtime=manager._runtime()
    require(runtime is not None,'Missing verified full-pack graphics runtime.')
    game=core._plain(core.validate_game(args.game).parent)
    require(not game.is_relative_to(manager.state_dir) and not manager.state_dir.is_relative_to(game),'Data must be outside game.')
    manager.base.mkdir(parents=True,exist_ok=True)
    binding=shortcut_spec(dict(vk=args.hotkey_vk,modifiers=args.hotkey_modifiers))
    if args.install:install(manager,game,runtime,args.dual_load=='1',binding)
    elif args.uninstall:uninstall(manager,game)
    elif args.shortcut:set_shortcut(manager,game,binding)
    else:watch(manager,game,runtime)


if __name__=='__main__':
    try:main()
    except Exception as error:
        print('Graphics operation refused: '+str(error),file=sys.stderr,flush=True)
        raise SystemExit(2)
