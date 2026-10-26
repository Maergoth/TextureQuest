"""New self-update UI/CLI integration: fake Tk/backend/processes, D data only."""
from contextlib import ExitStack,redirect_stderr
from io import StringIO
from pathlib import Path
import queue,sys,tempfile,threading,types,unittest
from unittest.mock import MagicMock,patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
import texturequest_app as module

class Widget:
    quest_primary=False
    def __init__(self,**values):self.values=dict(value=0,**values);self.visible=True
    def configure(self,**values):self.values.update(values)
    def cget(self,key):return self.values.get(key,'')
    def grid(self,*args,**kwargs):self.visible=True
    def grid_remove(self):self.visible=False
    def start(self,*args):pass
    def stop(self):pass
    def entryconfigure(self,*args,**kwargs):pass

class Root:
    def __init__(self):self.destroyed=False;self.idle=[];self.timers=[];self.visible=True;self.order=[]
    def destroy(self):self.destroyed=True
    def after(self,delay,callback):self.timers.append((delay,callback))
    def after_idle(self,callback):self.idle.append(callback)
    def update_idletasks(self):self.order.append('layout')
    def winfo_viewable(self):return self.visible
    def withdraw(self):self.visible=False
    def mainloop(self):
        for callback in self.idle:callback()

def fake_app(folder):
    app=module.TextureQuestApp.__new__(module.TextureQuestApp);app.root=Root();app.state_dir=folder/'data';app.state_dir.mkdir(exist_ok=True)
    app.app_version='1.3.5';app.busy=None;app.events=queue.Queue();app.pause=threading.Event();app.closing=False;app.close_after_pause=False
    app.overall_active=False;app.last_progress=app.last_overall_progress=0;app.prepared=app.preflight=None;app.known_pack_versions={};app.installed={};app.release_checked=True
    app.release=dict(schema='texturequest-release-v3',texture_version='1.2.3',version='1.3.7',archives=[],texture_count=0)
    app.update_check={'installer':dict(status='verified',update_available=True,version='1.3.6',bytes=10,sha256='a'*64)}
    app.game=types.SimpleNamespace(get=lambda:'')
    for name in ('game_entry','menu_button','install_button','launch_button','version_label','app_update_label','pack_update_label','installer_update_button','pack_versions_label','pack_description','download_label','download_button','install_label','menu','progress','overall_label','pause_button'):
        setattr(app,name,Widget())
    app.controls=[app.install_button,app.launch_button,app.download_button,app.installer_update_button]
    app.messages=[];app.set_message=lambda text,error=False:app.messages.append((text,error))
    return app

class AppSelfUpdateUI(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent,prefix='app-self-update-ui-');self.addCleanup(self.temp.cleanup)
        self.folder=Path(self.temp.name);self.exe=self.folder/'TextureQuest.exe';self.exe.write_bytes(b'Own non-executable fixture')
        self.app=fake_app(self.folder);self.request=dict(request_path=str(self.folder/'request.json'),nonce='b'*32,target_version='1.3.6',app_dir=str(self.folder),data_dir=str(self.app.state_dir))
        self.ready=dict(self.request,status='helper_ready')
        self.backend=types.SimpleNamespace(stage_update=MagicMock(return_value=self.request),launch_updater=MagicMock(return_value=self.ready),apply_update=MagicMock(return_value=0),acknowledge_startup=MagicMock(),failure_message=MagicMock(return_value='The old app was restored after the update failed.'))
        self.stack=ExitStack();self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.dict(sys.modules,{'texturequest_self_update':self.backend}))
        self.stack.enter_context(patch.object(module.sys,'frozen',True,create=True));self.stack.enter_context(patch.object(module.sys,'executable',str(self.exe)))
        self.stack.enter_context(patch.object(module,'game_executable',side_effect=ValueError('No selected game')))
        self.work=[]
        def thread(*,target,daemon):
            self.assertTrue(daemon);self.work.append(target);return types.SimpleNamespace(start=lambda:None)
        self.stack.enter_context(patch.object(module.threading,'Thread',side_effect=thread))

    def test_one_click_runs_in_background_and_closes_only_after_matching_helper_ready(self):
        sentinel=self.app.state_dir/'saved-textures.marker';sentinel.write_bytes(b'keep')
        self.assertTrue(self.app.open_app_update());self.assertEqual(self.app.busy,'app-update');self.assertEqual(self.app.installer_update_button.cget('state'),'disabled')
        self.backend.stage_update.assert_not_called();self.assertFalse(self.app.root.destroyed)
        self.work[0]();self.backend.stage_update.assert_called_once();self.backend.launch_updater.assert_called_once_with(self.request)
        kwargs=self.backend.stage_update.call_args.kwargs
        self.assertEqual(kwargs['app_dir'],self.folder);self.assertEqual(kwargs['data_dir'],self.app.state_dir);self.assertEqual(kwargs['current_version'],'1.3.5')
        self.assertFalse(self.app.root.destroyed);self.app.poll();self.assertTrue(self.app.root.destroyed)
        self.assertTrue(self.app.closing);self.assertEqual(sentinel.read_bytes(),b'keep');self.assertIn('Restarting TextureQuest',self.app.messages[-1][0])

    def test_download_staging_or_helper_failure_keeps_current_gui_and_visible_error(self):
        for stage,launch in ((OSError('download refused'),None),(ValueError('staging refused'),None),(None,OSError('helper launch refused'))):
            app=fake_app(self.folder);self.backend.stage_update.reset_mock();self.backend.launch_updater.reset_mock()
            self.backend.stage_update.side_effect=stage;self.backend.launch_updater.side_effect=launch
            self.assertTrue(app.open_app_update());self.work[-1]();app.poll()
            self.assertFalse(app.root.destroyed);self.assertFalse(app.closing);self.assertIsNone(app.busy);self.assertTrue(app.messages[-1][1])
            if stage:self.backend.launch_updater.assert_not_called()

    def test_foreign_or_missing_helper_success_does_not_close(self):
        for ready in (None,dict(self.ready,nonce='f'*32)):
            app=fake_app(self.folder);self.backend.launch_updater.return_value=ready
            app.open_app_update();self.work[-1]();app.poll();self.assertFalse(app.root.destroyed);self.assertTrue(app.messages[-1][1])

    def test_busy_update_button_and_other_controls_refuse_concurrent_operations(self):
        for busy in ('download','update','install','verify','app-update'):
            self.app.busy=busy;self.app.refresh_controls()
            self.assertEqual(self.app.installer_update_button.cget('state'),'disabled');self.assertFalse(self.app.open_app_update())
            self.assertEqual(self.app.download_button.cget('state'),'disabled');self.assertEqual(self.app.launch_button.cget('state'),'disabled')
        self.backend.stage_update.assert_not_called();self.app.busy=None;self.app.refresh_controls();self.assertEqual(self.app.installer_update_button.cget('state'),'normal')

    def test_app_progress_does_not_overwrite_texture_progress_or_close(self):
        self.app.busy='app-update';self.app.download_label.configure(text='Saved texture files verified')
        self.app.events.put(('app-update-progress',dict(phase='download',message='Downloading verified app',done=25,total=100)))
        self.app.poll();self.assertEqual(self.app.progress.cget('value'),25);self.assertIn('25.0%',self.app.overall_label.cget('text'))
        self.assertEqual(self.app.download_label.cget('text'),'Saved texture files verified');self.assertFalse(self.app.root.destroyed)

    def test_helper_cli_dispatch_precedes_tk_and_rejects_missing_nonce(self):
        with patch.object(module.tk,'Tk') as create,patch.object(module,'TextureQuestApp') as app:
            self.assertEqual(module.main(['--apply-app-update',self.request['request_path'],'--update-nonce',self.request['nonce']]),0)
            create.assert_not_called();app.assert_not_called()
        self.backend.apply_update.assert_called_once_with(Path(self.request['request_path']),self.request['nonce'],smoke_test=False)
        self.backend.apply_update.reset_mock()
        with patch.object(module.tk,'Tk') as create,redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit):module.main(['--apply-app-update',self.request['request_path']])
            create.assert_not_called()
        self.backend.apply_update.assert_not_called()

    def test_restart_ack_is_after_initialization_and_uses_exact_app_data_and_nonce(self):
        root=Root();app=self.app;app.root=root;order=[]
        def initialized(*args,**kwargs):order.append('initialized');return app
        self.backend.acknowledge_startup.side_effect=lambda *args,**kwargs:order.append('ack')
        with patch.object(module.tk,'Tk',return_value=root),patch.object(module,'TextureQuestApp',side_effect=initialized):
            module.main(['--data-dir',str(app.state_dir),'--app-update-ack',self.request['request_path'],'--update-nonce',self.request['nonce']])
        self.assertEqual(order,['initialized','ack']);self.assertIn('layout',root.order)
        self.backend.acknowledge_startup.assert_called_once_with(Path(self.request['request_path']),self.request['nonce'],app_dir=self.folder,data_dir=app.state_dir,version='1.3.5')

    def test_invisible_startup_waits_ack_error_is_visible_and_rollback_notice_never_acks(self):
        self.app.root.visible=False;module.finish_app_update_startup(self.app,self.request['request_path'],self.request['nonce'])
        self.backend.acknowledge_startup.assert_not_called();self.assertEqual(self.app.root.timers[-1][0],100)
        self.app.root.visible=True;self.backend.acknowledge_startup.side_effect=ValueError('nonce refused')
        module.finish_app_update_startup(self.app,self.request['request_path'],self.request['nonce']);self.assertIn('nonce refused',self.app.messages[-1][0]);self.assertFalse(self.app.root.destroyed)
        self.backend.acknowledge_startup.reset_mock();module.finish_app_update_startup(self.app,self.request['request_path'],self.request['nonce'],failed=True)
        self.backend.acknowledge_startup.assert_not_called();self.backend.failure_message.assert_called_once();self.assertTrue(self.app.messages[-1][1])

    def test_source_checkout_reports_manual_bootstrap_without_browser_or_backend(self):
        with patch.object(module.sys,'frozen',False):self.assertFalse(self.app.open_app_update())
        self.assertIn('distributed TextureQuest app',self.app.messages[-1][0]);self.backend.stage_update.assert_not_called()

    def test_existing_smoke_flag_reaches_helper_without_gui_or_arbitrary_restart_args(self):
        with patch.object(module.tk,'Tk') as create:
            self.assertEqual(module.main(['--apply-app-update',self.request['request_path'],'--update-nonce',self.request['nonce'],'--smoke-test']),0)
            create.assert_not_called()
        self.backend.apply_update.assert_called_once_with(Path(self.request['request_path']),self.request['nonce'],smoke_test=True)
        self.backend.apply_update.reset_mock()
        with patch.object(module.tk,'Tk') as create,redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit):module.main(['--apply-app-update',self.request['request_path'],'--update-nonce',self.request['nonce'],'--data-dir',str(self.app.state_dir)])
            create.assert_not_called()
        self.backend.apply_update.assert_not_called()

    def test_rollback_startup_keeps_failure_visible_without_automatic_update_check(self):
        root=Root();app=self.app;app.root=root
        with patch.object(module.tk,'Tk',return_value=root),patch.object(module,'TextureQuestApp',return_value=app) as initialized:
            module.main(['--data-dir',str(app.state_dir),'--app-update-failed',self.request['request_path'],'--update-nonce',self.request['nonce']])
        self.assertFalse(initialized.call_args.kwargs['startup_check']);self.backend.acknowledge_startup.assert_not_called()
        self.assertIn('old app was restored',app.messages[-1][0]);self.assertTrue(app.messages[-1][1])
        self.assertEqual(root.timers,[]);self.assertFalse(root.destroyed)

if __name__=='__main__':unittest.main()
