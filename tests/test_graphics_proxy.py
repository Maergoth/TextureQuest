"""New graphics-only installer tests: invented archives, no game/native calls."""
from pathlib import Path
import hashlib
import json
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
import texturequest_graphics_proxy as g
from texture_swapper import core


def dds(n, value):
    words=[0]*31
    words[0],words[1],words[2],words[3],words[6],words[18]=124,0x21007,n,n,1,32
    words[19:26]=[0x41,0,32,0xff0000,0xff00,0xff,0xff000000]
    words[26]=0x1000
    return b'DDS '+struct.pack('<31I',*words)+bytes([value,4,7,255])*(n*n)


class FixtureManager(g.GraphicsManager):
    def _games_open(self,game): return 0
    def _exclusive_mode(self,game): pass
    def _prepare_cache(self,game,home,runtime,report):
        old,new=dds(4,10),dds(8,99)
        source=home/'source.vpk';target=home/'target.vpk'
        source.write_bytes(b'invented source');target.write_bytes(b'invented target')
        oldpin=lambda p:dict(path=str(p),bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())
        prep=home/'ready';(prep/'receipts').mkdir(parents=True)
        decision=dict(action='RELEASED_SOURCE_COMPATIBLE',member='fixture.dds',observed=dict(bytes=len(old),
            sha256=hashlib.sha256(old).hexdigest(),target=dict(bytes=len(new),sha256=hashlib.sha256(new).hexdigest())))
        receipt=dict(name='fixture.vpk',source=oldpin(source),output=oldpin(target),decisions=[decision])
        (prep/'receipts/fixture.json').write_bytes(g.catalog.canonical(receipt))
        ready=dict(schema='tq-fullpack-ready-v1',game=str(game),unavailable=[],applied=1,provider_count=1,
            providers=[dict(name='fixture.vpk',source=oldpin(source),output=oldpin(target),applied=1,receipt='receipts/fixture.json')])
        (prep/'READY.json').write_bytes(g.catalog.canonical(ready))
        api=SimpleNamespace(index=lambda f,n:SimpleNamespace(entries=[SimpleNamespace(member='fixture.dds')]),
            member_data=lambda f,e:old if f.read()==b'invented source' else new)
        offline=SimpleNamespace(_runtime=lambda:dict(portable_pending=True,bundle='inert'),
            _materialize=lambda bundle,game:dict(preparation_root=str(self.state_dir/'sideload/runtimes'/('f'*64)/'background-epochs')))
        ready_result=dict(ready_json=dict(path=str(prep/'READY.json')),applied=1)
        with patch.object(g,'LoaderManager',return_value=offline),patch.object(g,'_prepare',return_value=ready_result),patch.object(g,'vpk',api):
            return super()._prepare_cache(game,home,runtime,report)



class ProxyTests(unittest.TestCase):
    def setUp(self):
        root=Path(r'D:\eq2 paks\work\tmp');root.mkdir(exist_ok=True)
        self.tmp=tempfile.TemporaryDirectory(prefix='gpx-',dir=root);self.root=Path(self.tmp.name)
        self.game=self.root/'game';(self.game/'paks').mkdir(parents=True)
        (self.game/'EverQuest2.exe').write_bytes(b'inert EXE, never executed')
        (self.game/'ReShade.ini').write_bytes(b'[RENODX]\nPreserve=1\n')
        (self.game/'paks/unchanged.vpk').write_bytes(b'unchanged')
        self.state=self.root/'data';self.state.mkdir();self.rt=self.root/'runtime';self.rt.mkdir()
        files=[]
        for name in g.NATIVE:
            p=self.rt/name;p.write_bytes(('inert '+name).encode());files.append(dict(name=name,bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
        doc=dict(schema='texturequest-graphics-runtime-v2',capabilities=list(g.CAPABILITIES),files=files,
            canonical_role='stock',archive_redirection=False,background_process=False,full_pack_prepared=True,
            config_schema='TQGXCFG2',index_schema='TQGX0002')
        (self.rt/'RUNTIME.json').write_bytes(g.catalog.canonical(doc))
        self.prepared=dict(schema='texturequest-direct-prepared-v1',inert=True)
        core._save(self.state/'direct-prepared.json',self.prepared)
        self.m=FixtureManager(self.state,runtime_dir=self.rt)
        self.closed=patch.object(g,'ensure_closed');self.closed.start()

    def tearDown(self):
        self.closed.stop()
        assert self.root.resolve().is_relative_to(Path(r'D:\eq2 paks\work\tmp').resolve())
        self.tmp.cleanup()

    def install(self): return self.m.install(self.game,self.prepared)
    def unchanged(self):
        self.assertEqual((self.game/'ReShade.ini').read_bytes(),b'[RENODX]\nPreserve=1\n')
        self.assertEqual((self.game/'paks/unchanged.vpk').read_bytes(),b'unchanged')
        self.assertEqual((self.game/'EverQuest2.exe').read_bytes(),b'inert EXE, never executed')

    def test_normal_install_uses_stock_catalog_and_only_two_additions(self):
        result=self.install();self.assertTrue(result['installed']);self.assertFalse(result['running'])
        d=self.m._record(self.game);home=Path(d['candidate']);raw=(home/'addons/catalog.cfg').read_bytes()
        self.assertEqual(raw[:8],b'TQGXCFG2');self.assertEqual(struct.unpack_from('<II',raw,8),(2,0))
        self.assertEqual({p.name for p in self.game.iterdir()},{'paks','EverQuest2.exe','ReShade.ini','d3d11.dll','TextureQuest.ini'})
        self.assertEqual((home/'shortcut.txt').read_bytes(),b'TQKEY1\n106\n0\n')
        self.assertIn(str(home/'addons').lower().encode(),(home/'ReShade.ini').read_bytes())
        self.assertEqual(self.m.installed_game(),str(self.game));self.assertTrue(self.m.cache_in_use());self.unchanged()

    def test_uninstall_reinstall_preserves_cache_and_history(self):
        self.install();old=self.m._record(self.game);home=Path(old['candidate'])
        self.assertFalse(self.m.uninstall(self.game)['installed']);self.assertTrue((home/'textures/payloads.bin').exists())
        self.assertTrue(list((home/'uninstall-history').glob('*.json')));self.assertFalse(self.m.cache_in_use())
        self.install();self.assertNotEqual(self.m._record(self.game)['id'],old['id']);self.unchanged()

    def test_shortcut_updates_live_private_key_without_dll_or_game_changes(self):
        self.install();dll=(self.game/'d3d11.dll').read_bytes();d=self.m._record(self.game)
        result=self.m.set_shortcut(self.game,dict(vk=0x77,modifiers=2))
        self.assertEqual(result['shortcut'],dict(vk=0x77,modifiers=2))
        self.assertEqual((Path(d['candidate'])/'shortcut.txt').read_bytes(),b'TQKEY1\n119\n2\n')
        self.assertEqual((self.game/'d3d11.dll').read_bytes(),dll);self.unchanged()

    def test_foreign_proxy_is_never_overwritten(self):
        (self.game/'d3d11.dll').write_bytes(b'foreign')
        with self.assertRaises(ValueError): self.install()
        self.assertEqual((self.game/'d3d11.dll').read_bytes(),b'foreign')

    def test_existing_dxgi_or_owned_name_config_refuses(self):
        for name in ['dxgi.dll','TextureQuest.ini']:
            p=self.game/name;p.write_bytes(b'foreign')
            with self.assertRaises(ValueError):self.install()
            self.assertEqual(p.read_bytes(),b'foreign');p.unlink()

    def test_runtime_tamper_refuses(self):
        (self.rt/'d3d11.dll').write_bytes(b'changed')
        with self.assertRaises(RuntimeError):self.install()

    def test_catalog_direction_descriptor_mismatch_refuses(self):
        p=self.rt/'RUNTIME.json';d=json.loads(p.read_bytes());d['canonical_role']='upscaled';p.write_bytes(g.catalog.canonical(d))
        with self.assertRaises(ValueError):self.install()

    def test_dual_load_refuses(self):
        with self.assertRaises(ValueError):self.m.install(self.game,self.prepared,dual_load=True)

    def test_game_open_refuses(self):
        with patch.object(g,'ensure_closed',side_effect=ValueError('inert open game')):
            with self.assertRaises(ValueError):self.install()
        self.assertFalse((self.game/'d3d11.dll').exists())

    def test_changed_download_metadata_refuses(self):
        with self.assertRaises(ValueError):self.m.install(self.game,dict(other=True))

    def test_changed_proxy_is_preserved_during_uninstall(self):
        self.install();(self.game/'d3d11.dll').write_bytes(b'foreign replacement')
        with self.assertRaises(RuntimeError):self.m.uninstall(self.game)
        self.assertTrue((self.game/'TextureQuest.ini').exists());self.unchanged()

    def test_partial_copy_has_recoverable_owned_record(self):
        real=Path.open
        def fail_dll(p,*args,**kw):
            if p==self.game/'d3d11.dll' and args and args[0]=='xb':raise OSError('inert disk failure')
            return real(p,*args,**kw)
        with patch.object(Path,'open',fail_dll):
            with self.assertRaises(OSError):self.install()
        self.assertEqual(self.m._record(self.game)['phase'],'failed')
        self.assertTrue(self.m.status(self.game)['installed'])
        self.assertFalse(self.m.uninstall(self.game)['installed']);self.unchanged()

    def test_patcher_removed_proxy_can_be_uninstalled_and_reinstalled(self):
        self.install();(self.game/'d3d11.dll').unlink()
        self.assertIn('missing',self.m.status(self.game)['message'])
        self.m.uninstall(self.game);self.install();self.unchanged()

    def test_ascii_path_refusal_before_any_game_writes(self):
        self.m.cache_base=self.root/'café'
        with self.assertRaisesRegex(ValueError,'ASCII'):self.install()
        self.assertFalse((self.game/'d3d11.dll').exists())

    def test_prototype_record_without_shortcut_is_recognized(self):
        self.install();p=self.m._record_path(self.game);d=json.loads(p.read_bytes());d.pop('shortcut')
        d['hotkey']='Numpad *';d['record_sha256']=core._digest({k:v for k,v in d.items() if k!='record_sha256'})
        p.write_bytes(g.catalog.canonical(d))
        self.assertEqual(self.m.status(self.game)['shortcut'],dict(vk=106,modifiers=0))
        self.assertEqual(self.m.installed_game(),str(self.game))
        self.m.uninstall(self.game);self.unchanged()

    def test_record_cannot_redirect_uninstall_to_unrelated_file(self):
        self.install();p=self.m._record_path(self.game);d=json.loads(p.read_bytes())
        d['files'][0]['path']=str(self.game/'ReShade.ini')
        d['record_sha256']=core._digest({k:v for k,v in d.items() if k!='record_sha256'});p.write_bytes(g.catalog.canonical(d))
        with self.assertRaises(ValueError):self.m.uninstall(self.game)
        self.unchanged()

    def test_pak_engine_rechecks_proxy_ownership_beneath_its_locks(self):
        from texturequest_direct import DirectSwapper
        import texturequest_sideload as legacy
        import texturequest_graphics as graphics
        inactive=SimpleNamespace(cache_in_use=lambda:False)
        active=SimpleNamespace(cache_in_use=lambda:True)
        with patch.object(legacy,'LoaderManager',return_value=inactive),patch.object(graphics,'GraphicsManager',return_value=active):
            with self.assertRaisesRegex(RuntimeError,'Graphics hook'):
                DirectSwapper(self.state)._assert_background_inactive()
        self.assertFalse((self.game/'d3d11.dll').exists());self.unchanged()

    def test_legacy_uninstall_only_signals_exact_owned_event(self):
        home=self.root/'old-home';home.mkdir();legacy=dict(schema='texturequest-graphics-installation-v1',game=str(self.game),phase='installed',
            installation_id='a'*32,home=str(home),runtime_dir=str(self.rt),sessions=[],watcher=dict(pid=123,birth100ns=456,exe='inert'),
            stop_event='Local\\TQWatch-'+'a'*32,startup_name='TextureQuestGraphics-'+self.m._key(self.game),startup_command='inert old command')
        legacy['record_sha256']=core._digest(legacy);p=self.m._legacy_path(self.game);p.parent.mkdir(parents=True);p.write_bytes(g.catalog.canonical(legacy))
        with patch.object(g,'_alive',side_effect=[True,False,False]),patch.object(g,'_signal_stop') as signal,patch.object(g,'_remove_run') as remove:
            self.m.uninstall(self.game);signal.assert_called_once_with(legacy['stop_event']);remove.assert_called_once_with(legacy['startup_name'],legacy['startup_command'])
        self.assertEqual(self.m._legacy(self.game)['phase'],'uninstalled');self.unchanged()


if __name__=='__main__':unittest.main(verbosity=2)
