"""Install a normal Direct3D proxy; never inject into or launch the game.

Archive work below is offline cache preparation. The game runtime receives only
the graphics provider, add-on, profile companion, catalog, and hotkey setting.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import struct
import time
import uuid

from texture_swapper import core
from texturequest_sideload import (LoaderManager, _prepare, _pin, _json, _alive,
    _live_status_bytes, _remove_run, _signal_stop, _read_run)
from texturequest_processes import ensure_closed
from texturequest_shortcut import normalize, label
import texturequest_graphics_catalog as catalog
import texturequest_graphics_vpk as vpk

CAPABILITIES = {'full_pack','on_demand','foreground_toggle','bounded_cache',
                'custom_shortcut','normal_proxy','graphics_only'}
NATIVE = {'d3d11.dll','TextureQuestGraphicsHost.addon64','TextureQuestGraphicsBootstrap.dll','LICENSE-ReShade.md'}


def need(ok, why):
    if not ok: raise ValueError(why)


def exclusive(path, value):
    with core._plain(path).open('xb') as f:
        f.write(catalog.canonical(value)+b'\n'); f.flush(); os.fsync(f.fileno())


def profile(base):
    addons = str(base/'addons').lower()
    need(not any(c in str(base) for c in '\r\n,='), 'Choose an app/cache path without commas or equals signs.')
    return ('[INSTALL]\nMaxEffectRuntimes=0\n[GENERAL]\nDisable=1\n[ADDON]\nAddonPath='+
        addons+'\nDisabledAddons=Generic Depth,Effect Runtime Sync\n[PROXY]\nEnableProxyLibrary=0\n').encode()


class GraphicsManager:
    def __init__(self, state_dir, *, runtime_dir=None):
        self.state_dir = core._plain(state_dir)
        self.runtime_dir = Path(runtime_dir) if runtime_dir else Path(__file__).resolve().parent/'graphics-runtime'
        self.base = self.state_dir/'graphics'
        self.records = self.state_dir/'graphics-proxy'
        self.cache_base = self.state_dir/'graphics-cache'
        location = self.state_dir/'graphics-cache-location.json'
        if location.exists():
            value = core._load(location)
            need(value.get('schema')=='texturequest-graphics-cache-location-v1','Unknown graphics cache location.')
            self.cache_base = core._plain(value['cache_root'])
        self.runtime_cache = None

    def _runtime(self):
        if self.runtime_cache:
            doc, stamps = self.runtime_cache
            if all(tuple(catalog.stat5(p.stat()))==s for p,s in stamps): return doc
        path = self.runtime_dir/'RUNTIME.json'
        if not path.exists(): return None
        doc = _json(path)
        need(doc.get('schema')=='texturequest-graphics-runtime-v2' and
             set(doc.get('capabilities',[]))==CAPABILITIES and doc.get('canonical_role')=='stock' and
             doc.get('archive_redirection') is False and doc.get('background_process') is False and
             doc.get('full_pack_prepared') is True and doc.get('config_schema')=='TQGXCFG2' and doc.get('index_schema')=='TQGX0002',
             'Unknown graphics-only runtime.')
        files = doc.get('files')
        need(type(files) is list and len(files)==len(NATIVE) and
             {x.get('name') for x in files}==NATIVE, 'Incomplete/aliased graphics runtime.')
        paths = [path]
        for row in files:
            p = self.runtime_dir/row['name']; _pin(p,row); paths.append(p)
        self.runtime_cache = doc, [(p,tuple(catalog.stat5(p.stat()))) for p in paths]
        return doc

    def _key(self, game):
        return hashlib.sha256(str(core._plain(game)).casefold().encode()).hexdigest()[:24]

    def _record_path(self, game): return self.records/(self._key(game)+'.json')
    def _legacy_path(self, game): return self.base/'registrations'/(self._key(game)+'.json')

    def _read(self, path, game, proxy):
        raw = _live_status_bytes(path)
        if raw is None: return None
        d = core._json(raw)
        need(d.get('record_sha256')==core._digest({k:v for k,v in d.items() if k!='record_sha256'}) and
             str(core._plain(d['game'])).casefold()==str(core._plain(game)).casefold(), 'Graphics record changed; preserve it for recovery.')
        if proxy:
            need(d.get('schema')=='texturequest-graphics-proxy-installation-v1' and
                 d.get('phase') in {'installed','uninstalled','preparing','failed'} and
                 re.fullmatch('[a-f0-9]{32}',d.get('id','')) is not None and
                 type(d.get('files')) is list and len(d['files'])==2, 'Unknown graphics proxy record.')
            need({str(core._plain(x['path'])).casefold() for x in d['files']}==
                 {str(core._plain(game)/n).casefold() for n in ['d3d11.dll','TextureQuest.ini']}, 'Proxy file scope changed.')
            home = core._plain(d['candidate'])
            need(not home.is_relative_to(core._plain(game)), 'Proxy cache must be outside the game.')
        else:
            need(d.get('schema')=='texturequest-graphics-installation-v1' and
                 re.fullmatch('[a-f0-9]{32}',d.get('installation_id','')) is not None,
                 'Unknown legacy graphics record.')
        return d

    def _record(self, game): return self._read(self._record_path(game),game,True)
    def _legacy(self, game): return self._read(self._legacy_path(game),game,False)

    def _save(self, game, doc, expected):
        path = self._record_path(game)
        actual = _live_status_bytes(path)
        need(actual==expected,'Graphics registration changed during this operation.')
        doc['record_sha256']=core._digest({k:v for k,v in doc.items() if k!='record_sha256'})
        core._atomic(path,doc)
        return path.read_bytes()

    def _games_open(self, game):
        import psutil
        target = str(core._plain(game)/'EverQuest2.exe').casefold()
        count=0
        for p in psutil.process_iter(['pid','name']):
            if (p.info['name'] or '').casefold()=='everquest2.exe':
                try: count += p.exe().casefold()==target
                except psutil.NoSuchProcess: pass
        return count

    def status(self, game):
        runtime = self._runtime()
        result=dict(installed=False,running=False,active_sessions=0,available=runtime is not None,
            dual_load=False,shortcut=normalize(),message='Graphics-only runtime is not included.')
        if not str(game).strip(): return result
        record=self._record(game)
        if record and record['phase']!='uninstalled':
            binding=normalize(record.get('shortcut'))
            missing=any(not Path(x['path']).is_file() for x in record['files'])
            msg=('Hook file missing. Close EQ2, then Uninstall and Install to repair.' if missing and record['phase']=='installed' else
                 'Graphics-only hook installed. '+label(binding)+' switches quality. No background process.'
                 if record['phase']=='installed' else 'Interrupted graphics installation. Close EQ2 and Uninstall before retrying.')
            return dict(result,installed=True,active_sessions=self._games_open(game),
                available=True,shortcut=binding,message=msg)
        legacy=self._legacy(game)
        if legacy:
            sessions=legacy.get('sessions',[])
            count=sum(bool(_alive(x,x['exe'])) for x in sessions)
            watcher=legacy.get('watcher')
            running=bool(watcher and _alive(watcher,Path(legacy['runtime_dir'])/'TextureQuestGraphics.exe'))
            if legacy['phase']!='uninstalled' or running or count:
                return dict(result,installed=legacy['phase']!='uninstalled',running=running,active_sessions=count,
                    available=True,message='Old injected graphics hook. Close EQ2 and Uninstall here, then Install the graphics-only version.')
        if runtime: result['message']='Graphics-only textures. '+label()+' switches Original/Upscaled. Close EQ2 to install.'
        return result

    def _all_games(self):
        found={}
        for folder in [self.records,self.base/'registrations']:
            if not folder.exists(): continue
            for p in core._plain(folder).iterdir():
                need(p.is_file() and re.fullmatch(r'[a-f0-9]{24}\.json',p.name), 'Unknown graphics state file; cache preserved.')
                d=_json(p);found[str(core._plain(d['game'])).casefold()]=d['game']
        return list(found.values())

    def cache_in_use(self):
        return any(any(self.status(g)[k] for k in ['installed','running','active_sessions']) for g in self._all_games())

    def installed_game(self):
        games=[g for g in self._all_games() if self.status(g)['installed']]
        need(len(games)<=1,'Multiple graphics installations; choose a game folder explicitly.')
        return games[0] if games else None

    def _exclusive_mode(self, game):
        need(not LoaderManager(self.state_dir).cache_in_use(),'Uninstall the old Memory hook and close its sessions first.')
        legacy=self._legacy(game)
        need(not legacy or (legacy['phase']=='uninstalled' and not self.status(game)['running'] and
             not self.status(game)['active_sessions']), 'Uninstall the old injected graphics hook first.')

    def _prepare_cache(self, game, home, runtime, report):
        # This is reviewed offline Python archive preparation only. None of its
        # legacy watcher/DLL resources are started or loaded into the game.
        memory=LoaderManager(self.state_dir)
        memory.descriptor=self.runtime_dir.parent/'texturequest-sideload-runtime.json'
        admitted=memory._runtime(); need(admitted is not None and admitted.get('portable_pending'),'Offline archive preparation tools missing.')
        component=memory._materialize(admitted['bundle'],game)
        prep_root=home/'offline';prep_root.mkdir()
        prepared=_prepare(component,game,self.state_dir/'direct-prepared.json',prep_root/'pack',report)
        summary=catalog.prepare(Path(prepared['ready_json']['path']),home/'textures',vpk,
            lambda text,*_:report(text),direction='stock_canonical')
        need(summary['rows']==prepared['applied'] and not summary['gaps'],'Unsupported graphics formats remain; preparation preserved.')
        old=(home/'textures/catalog.bin').read_bytes()
        need(old[:8]==b'TQGX0001' and hashlib.sha256(old).hexdigest()==summary['index']['sha256'],'Graphics index changed.')
        index=b'TQGX0002'+struct.pack('<I',2)+old[12:]
        payload=str(home/'textures/payloads.bin').encode()
        config=b'TQGXCFG2'+struct.pack('<4I',2,0,len(payload),len(index))+payload+index
        addons=home/'addons';addons.mkdir()
        (addons/'catalog.cfg').write_bytes(config)
        for name in ['TextureQuestGraphicsHost.addon64','TextureQuestGraphicsBootstrap.dll']:
            shutil.copy2(self.runtime_dir/name,addons/name)
        (home/'ReShade.ini').write_bytes(profile(home))
        return summary

    def _configure(self, game, home, runtime, binding, identity):
        addons=home/'addons'
        game_text=('[INSTALL]\nBasePath='+str(home).lower()+'\n').encode()
        (home/'game-TextureQuest.ini').write_bytes(game_text)
        self._write_key(home,binding)
        rows={x['name']:x for x in runtime['files']}
        fields=dict(installation=identity,game=str(game).lower().encode().hex(),base=str(home).lower().encode().hex(),
            host_sha256=rows['TextureQuestGraphicsHost.addon64']['sha256'],provider_sha256=rows['d3d11.dll']['sha256'],
            bootstrap=str(addons/'TextureQuestGraphicsBootstrap.dll').lower().encode().hex(),bootstrap_sha256=rows['TextureQuestGraphicsBootstrap.dll']['sha256'],
            catalog=str(addons/'catalog.cfg').lower().encode().hex(),catalog_sha256=hashlib.sha256((addons/'catalog.cfg').read_bytes()).hexdigest(),
            base_ini_sha256=hashlib.sha256((home/'ReShade.ini').read_bytes()).hexdigest(),game_ini_sha256=hashlib.sha256(game_text).hexdigest())
        (addons/'TextureQuestProxy.cfg').write_bytes(('TQGFXP1\n'+''.join(k+'='+v+'\n' for k,v in fields.items())).encode())
        return game_text

    def _write_key(self, home, binding):
        raw=('TQKEY1\n'+str(binding['vk'])+'\n'+str(binding['modifiers'])+'\n').encode()
        path=home/'shortcut.txt';tmp=home/('shortcut-'+uuid.uuid4().hex+'.tmp')
        with tmp.open('xb') as f: f.write(raw);f.flush();os.fsync(f.fileno())
        os.replace(tmp,path)

    def install(self, game, prepared, *, dual_load=False, progress=None, shortcut=None):
        need(dual_load is False,'Dual-load is not supported; replacements load on demand.')
        report=progress or (lambda _:None);binding=normalize(shortcut)
        game=core._plain(core.validate_game(game).parent);runtime=self._runtime();need(runtime,'Graphics runtime missing.')
        need(all(32<=ord(c)<127 for c in str(game)+str(self.cache_base)),
             'This graphics runtime needs game and cache paths with ASCII characters.')
        ensure_closed()
        need(not self.cache_base.is_relative_to(game) and not game.is_relative_to(self.cache_base),'Cache must be outside the game folder.')
        with core._lock(self.state_dir/'operation.lock'),core._lock(self.state_dir/'sideload/helper-owner.lock'):
            self._exclusive_mode(game)
            old=self._record(game);need(not old or old['phase']=='uninstalled','Uninstall Graphics hook before installing again.')
            need(core._load(self.state_dir/'direct-prepared.json')==prepared,'Downloaded-pack metadata changed.')
            need(not any((game/n).exists() for n in ['d3d11.dll','TextureQuest.ini','dxgi.dll']),
                 'An existing graphics proxy/config is present. Preserve it; remove or integrate that installation first.')
            identity=uuid.uuid4().hex;home=core._plain(self.cache_base/'proxy-installations'/identity)
            need(len(str(home/'sessions'/('f'*32)))<220,'Choose a shorter app/cache folder for the graphics hook.')
            home.mkdir(parents=True);expected=_live_status_bytes(self._record_path(game))
            exclusive(home/'REQUEST.json',dict(action='install_graphics_only',game=str(game),runtime=runtime,previous=old))
            report('Preparing graphics cache. Original game archives stay unchanged.')
            summary=self._prepare_cache(game,home,runtime,report)
            game_text=self._configure(game,home,runtime,binding,identity)
            provider=self.runtime_dir/'d3d11.dll';raw=provider.read_bytes()
            need(hashlib.sha256(raw).hexdigest()==next(x['sha256'] for x in runtime['files'] if x['name']=='d3d11.dll'),'Runtime changed during preparation.')
            sources=[(game/'TextureQuest.ini',game_text),(game/'d3d11.dll',raw)]
            files=[dict(path=str(p),bytes=len(data),sha256=hashlib.sha256(data).hexdigest()) for p,data in sources]
            doc=dict(schema='texturequest-graphics-proxy-installation-v1',phase='preparing',id=identity,game=str(game),candidate=str(home),
                files=files,shortcut=binding,archive_redirection=False,background_process=False,rows=summary['rows'],ambiguous=len(summary['ambiguous']))
            ensure_closed();need(not any(p.exists() for p,_ in sources),'A graphics proxy appeared during preparation.')
            expected=self._save(game,doc,expected)
            try:
                for p,data in sources:
                    ensure_closed()
                    with p.open('xb') as f: f.write(data);f.flush();os.fsync(f.fileno())
                    _pin(p,next(x for x in files if x['path']==str(p)))
                doc['phase']='installed';expected=self._save(game,doc,expected)
                exclusive(home/'COMMITTED.json',dict(record_sha256=doc['record_sha256'],files=files))
            except BaseException:
                doc['phase']='failed';self._save(game,doc,expected);raise
        report('Graphics-only hook installed. '+str(doc['ambiguous'])+' ambiguous entries stay original. '+label(binding)+' switches quality.')
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
        game=core._plain(core.validate_game(game).parent);ensure_closed()
        with core._lock(self.state_dir/'operation.lock'),core._lock(self.state_dir/'sideload/helper-owner.lock'):
            doc=self._record(game)
            if not doc or doc['phase']=='uninstalled':
                legacy=self._legacy(game);need(legacy and legacy['phase']!='uninstalled','No installed graphics hook.')
                self._uninstall_legacy(game,legacy)
            else:
                for row in doc['files']:
                    p=core._plain(row['path'])
                    if p.exists():_pin(p,row)
                    # The official patcher may remove one of our files. Missing is
                    # safe to remove; any present file must still match exactly.
                home=core._plain(doc['candidate']);history=home/'uninstall-history';history.mkdir(exist_ok=True)
                exclusive(history/(uuid.uuid4().hex+'.json'),doc)
                expected=_live_status_bytes(self._record_path(game))
                for row in sorted(doc['files'],key=lambda x:0 if x['path'].lower().endswith('.dll') else 1):
                    ensure_closed();p=core._plain(row['path'])
                    if p.exists():_pin(p,row);p.unlink()
                doc['phase']='uninstalled';self._save(game,doc,expected)
        if progress:progress('Graphics hook removed. Downloads and private cache kept.')
        return self.status(game)

    def set_shortcut(self, game, shortcut, progress=None):
        binding=normalize(shortcut);game=core._plain(game)
        with core._lock(self.state_dir/'operation.lock'):
            doc=self._record(game);need(doc and doc['phase']=='installed','Install Graphics hook before saving its shortcut.')
            expected=_live_status_bytes(self._record_path(game));home=core._plain(doc['candidate'])
            history=home/'shortcut-history';history.mkdir(exist_ok=True)
            exclusive(history/(uuid.uuid4().hex+'.json'),dict(before=doc.get('shortcut'),after=binding))
            self._write_key(home,binding);doc['shortcut']=binding;self._save(game,doc,expected)
        if progress:progress('Shortcut saved: '+label(binding)+'. Active only while EQ2 is focused.')
        return self.status(game)
