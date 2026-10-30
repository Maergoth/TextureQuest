"""New controller contracts with owned tiny files and fake process/registry leaves."""
from pathlib import Path
import io
import hashlib,json,struct,sys,tempfile,unittest
from unittest.mock import patch,MagicMock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
import texturequest_graphics_control as c
from texturequest_graphics import GraphicsManager,CAPABILITIES
from texture_swapper import core

class ControllerCases(unittest.TestCase):
 def setUp(self):
  root=Path(__file__).resolve().parents[1]/'build';root.mkdir(exist_ok=True)
  self.temp=tempfile.TemporaryDirectory(dir=root);self.root=Path(self.temp.name)
  self.game=self.root/'game';(self.game/'paks').mkdir(parents=True)
  (self.game/'EverQuest2.exe').write_bytes(b'owned synthetic executable; never executed')
  self.runtime=self.root/'graphics-runtime';self.runtime.mkdir()
  names=['TextureQuestGraphics.exe','TQGraphicsAttach.exe','ReShade64.dll','TextureQuestGraphicsHost.addon64','TextureQuestGraphicsBootstrap.dll']
  files=[]
  for name in names:
   p=self.runtime/name;p.write_bytes(name.encode());files.append(dict(name=name,bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
  self.doc=dict(schema='texturequest-graphics-runtime-v1',capabilities=sorted(CAPABILITIES),full_pack_prepared=True,controller='TextureQuestGraphics.exe',files=files)
  (self.runtime/'RUNTIME.json').write_text(json.dumps(self.doc))
  self.m=GraphicsManager(self.root/'data',runtime_dir=self.runtime);self.m.base.mkdir(parents=True)
 def tearDown(self):self.temp.cleanup()
 def record(self):
  return dict(schema='texturequest-graphics-installation-v1',phase='installed',installation_id='a'*32,game=str(self.game),dual_load=True,sessions=[],runtime_dir=str(self.runtime),home=str(self.root/'home'),watcher=None)
 def test_runtime_tamper_never_remains_admitted(self):
  self.assertEqual(self.m._runtime(),self.doc)
  (self.runtime/'ReShade64.dll').write_bytes(b'tampered')
  with self.assertRaises(Exception):self.m._runtime()
 def test_two_login_handshakes_keep_history_and_update_ready_pointer(self):
  r=self.record();Path(r['home']).mkdir()
  legacy=Path(r['home'])/'WATCHER-READY.json';legacy.write_bytes(b'preserved legacy handshake')
  first=dict(pid=111,birth100ns=222,exe=str(self.runtime/'TextureQuestGraphics.exe'))
  second=dict(first,pid=333,birth100ns=444)
  c.publish_watcher_ready(self.m,self.game,r,first)
  c.publish_watcher_ready(self.m,self.game,r,second)
  self.assertEqual(legacy.read_bytes(),b'preserved legacy handshake')
  self.assertEqual(len(list((Path(r['home'])/'watcher-history').glob('*.json'))),2)
  self.assertEqual(self.m._record(self.game)['watcher'],second)
 def test_closed_session_history_does_not_exhaust_lifetime_launch_limit(self):
  r=self.record();r['sessions']=[dict(pid=i+1,birth100ns=i+100,exe='old exact game') for i in range(128)]
  with patch.object(c,'_alive',return_value=False):c.archive_closed_sessions(r)
  self.assertEqual(r['sessions'],[])
  self.assertEqual(len(list((Path(r['home'])/'session-history').glob('*.json'))),128)
 def test_app_streams_progress_and_preserves_child_refusal(self):
  prepared=dict(schema='texturequest-direct-prepared-v1')
  self.m.state_dir.mkdir(exist_ok=True);core._save(self.m.state_dir/'direct-prepared.json',prepared)
  process=MagicMock();process.stdout=io.StringIO('Indexing Exp01.vpk\nexplicit synthetic refusal\n');process.wait.return_value=2
  events=[]
  with patch('texturequest_graphics.ensure_closed'),patch('texturequest_graphics.subprocess.Popen',return_value=process):
   with self.assertRaisesRegex(RuntimeError,'explicit synthetic refusal'):
    self.m.install(self.game,prepared,progress=events.append)
  self.assertIn('Indexing Exp01.vpk',events)
 def test_registration_seal_and_active_session_survive_uninstall(self):
  r=self.record();r['sessions']=[dict(pid=123,birth100ns=456,exe=str(self.game/'EverQuest2.exe'))]
  c.save_record(self.m,self.game,r)
  with patch('texturequest_graphics._alive',return_value=True):
   self.assertEqual(self.m.status(self.game)['active_sessions'],1)
   r['phase']='uninstalled';c.save_record(self.m,self.game,r)
   self.assertTrue(self.m.cache_in_use())
  raw=json.loads(self.m._record_path(self.game).read_bytes());raw['sessions']=[]
  self.m._record_path(self.game).write_text(json.dumps(raw))
  with self.assertRaises(ValueError):self.m._record(self.game)
 def test_attach_request_binds_exact_roles_and_unicode_paths(self):
  r=self.record();pins={}
  for role in ('dll','manifest','bindings','config'):
   p=self.root/(role+'é.bin');p.write_bytes(role.encode());pins[role]=c._pin(p)
  r['epoch']=dict(component=dict(dll=pins['dll']),named=dict(manifest=pins['manifest'],bindings=pins['bindings']),config=pins['config'])
  identity=dict(pid=123,birth100ns=456,exe=str(self.game/'EverQuest2.exe'))
  nonce,session,raw=c.attach_request(r,identity,self.doc,self.root/'sessions')
  fields=dict(line.split('=',1) for line in raw.decode('ascii').splitlines()[1:])
  self.assertEqual(bytes.fromhex(fields['bindings']).decode(),pins['bindings']['path'])
  self.assertEqual(fields['host_sha256'],next(x['sha256'] for x in self.doc['files'] if x['name']=='TextureQuestGraphicsHost.addon64'))
  self.assertFalse(session.exists());self.assertEqual(len(nonce),32)
  with self.assertRaises(ValueError):c.attach_request(r,dict(identity,exe=str(self.root/'foreign.exe')),self.doc,self.root/'sessions')
 def test_uninstall_only_owned_startup_and_watcher_without_game_kill(self):
  r=self.record();r.update(startup_name='owned',startup_command='exact command',stop_event='Local\\TQWatch-'+'a'*32,watcher=dict(pid=123,birth100ns=456))
  c.save_record(self.m,self.game,r)
  with patch.object(c,'_remove_run') as remove,patch.object(c,'_signal_stop') as stop,patch.object(c,'_alive',side_effect=[True,False]):
   c.uninstall(self.m,self.game)
  remove.assert_called_once_with('owned','exact command');stop.assert_called_once()
  self.assertEqual(self.m._record(self.game)['phase'],'uninstalled')
 def test_install_refuses_memory_owner_before_any_preparation(self):
  fake=MagicMock();fake.cache_in_use.return_value=True
  with patch.object(c,'ensure_closed'),patch.object(c,'memory_manager',return_value=fake),patch.object(c,'build_epoch') as prepare:
   with self.assertRaises(ValueError):c.install(self.m,self.game,self.doc,False)
   prepare.assert_not_called()
 def test_failed_prepare_retains_history_without_registry_or_game_actions(self):
  fake=MagicMock();fake.cache_in_use.return_value=False
  with patch.object(c,'ensure_closed'),patch.object(c,'memory_manager',return_value=fake),patch.object(c,'build_epoch',side_effect=ValueError('synthetic missing artifact')),patch.object(c,'_write_run') as register:
   with self.assertRaisesRegex(ValueError,'missing artifact'):c.install(self.m,self.game,self.doc,False)
   register.assert_not_called()
  r=self.m._record(self.game);self.assertEqual(r['phase'],'failed');self.assertTrue(Path(r['home']).exists())

if __name__=='__main__':unittest.main()
