"""Explicit local-build flow: invented D bundles, fake processes/Tk, no network."""
from contextlib import ExitStack, redirect_stderr, contextmanager
from io import StringIO
from pathlib import Path
import json, queue, sys, tempfile, types, unittest
from unittest.mock import MagicMock, patch
import texturequest_self_update as update
import texturequest_app as app
from test_app_self_update_backend import bundle, make_bundle, make_zip, Processes
from test_app_self_update_ui import Root, Widget


class LocalBuildInstall(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='local-build-'); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.target, self.caller = self.root/'installed', self.root/'candidate'
        self.old, self.new = bundle('1.3.5', b'O'), bundle('1.3.6', b'N')
        make_bundle(self.target, self.old); make_bundle(self.caller, self.new)
        (self.target/'data').mkdir(); self.marker = self.target/'data/texture.marker'; self.marker.write_bytes(b'preserve data')
        self.unknown = self.target/'personal.txt'; self.unknown.write_bytes(b'preserve unknown root')
        self.sentinels = {p:(p.stat().st_ino,p.read_bytes()) for p in (self.marker,self.unknown)}
        self.archive = self.root/'candidate.zip'; make_zip(self.archive,self.new)
        self.processes = Processes(self.target)
        self.bootstrap = dict(pid=111,birth=111.25,exe=str(self.caller/'TextureQuest.exe'))
        self.processes.current = self.bootstrap; self.processes.living = {111:self.bootstrap}
        self.stack = ExitStack(); self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(update.sys,'frozen',True,create=True))
        self.stack.enter_context(patch.object(update.sys,'executable',self.bootstrap['exe']))

    def stage(self,**kwargs):
        return update.stage_local_build_install(self.archive,app_dir=self.target,running_version='1.3.6',processes=self.processes,**kwargs)

    def preserved(self):
        for p,before in self.sentinels.items(): self.assertEqual((p.stat().st_ino,p.read_bytes()),before)

    def apply(self,request):
        self.processes.as_helper(request)
        with patch.object(update,'ACK_SECONDS',0.3):
            return update.apply_update(request['request_path'],request['nonce'],processes=self.processes)

    def test_explicit_local_request_binds_real_candidate_and_exact_zip(self):
        walked=[]; walk=update.os.walk
        with patch.object(update.os,'walk',side_effect=lambda p,**kw:(walked.append(Path(p)) or walk(p,**kw))): request=self.stage()
        value,_,_,_,stage=update._request(request['request_path'],request['nonce'])
        self.assertEqual(value['schema'],update.LOCAL_REQUEST_SCHEMA)
        self.assertEqual(value['parent'],self.bootstrap)
        self.assertEqual(value['bootstrap']['bundle'],value['new_bundle'])
        self.assertEqual(value['installer']['mode'],'local-build')
        self.assertNotIn('status',value['installer']);self.assertNotIn('download_url',value['installer'])
        self.assertEqual(update._bundle(stage/'install','1.3.6'),update._bundle(self.caller,'1.3.6'))
        self.assertNotIn(self.target,walked);self.assertFalse(any(self.target/'data'==p or self.target/'data' in p.parents for p in walked))
        self.preserved()

    def test_source_checkout_wrong_exe_and_wrong_running_version_refuse(self):
        with patch.object(update.sys,'frozen',False):
            with self.assertRaisesRegex(update.AppUpdateError,'packaged'): self.stage()
        with patch.object(update.sys,'executable',str(self.target/'TextureQuest.exe')):
            with self.assertRaisesRegex(update.AppUpdateError,'running packaged'): self.stage()
        with self.assertRaisesRegex(update.AppUpdateError,'version mismatch'):
            update.stage_local_build_install(self.archive,app_dir=self.target,running_version='1.3.7',processes=self.processes)
        self.assertFalse(list(self.root.glob('.TextureQuest-update-*')));self.preserved()

    def test_equal_or_older_local_build_refuses_without_target_changes(self):
        for version in ('1.3.6','1.3.7'):
            target=self.root/('installed-'+version);make_bundle(target,bundle(version,b'O'))
            with self.assertRaisesRegex(update.AppUpdateError,'newer'):
                update.stage_local_build_install(self.archive,app_dir=target,running_version='1.3.6',processes=self.processes)
        self.assertFalse(list(self.root.glob('.TextureQuest-update-*')));self.preserved()

    def test_overlapping_folders_and_other_running_app_refuse_before_stage(self):
        with self.assertRaisesRegex(update.AppUpdateError,'separate'):
            update.stage_local_build_install(self.archive,app_dir=self.caller,running_version='1.3.6',processes=self.processes)
        self.processes.refuse_other=True
        with self.assertRaisesRegex(update.AppUpdateError,'Other instance'):self.stage()
        self.assertFalse(list(self.root.glob('.TextureQuest-update-*')));self.preserved()

    def test_valid_but_different_local_zip_refuses_before_replacement(self):
        make_zip(self.archive,bundle('1.3.6',b'D'))
        with self.assertRaisesRegex(update.AppUpdateError,'exactly match'):self.stage()
        self.assertEqual(update._bundle(self.target,'1.3.5')['files']['TextureQuest.exe']['bytes'],len(self.old['TextureQuest.exe']))
        self.assertFalse(any((p/'install').exists() for p in self.root.glob('.TextureQuest-update-*')));self.preserved()

    def test_local_request_cannot_be_confused_with_official_or_extra_identity(self):
        request=self.stage();path=Path(request['request_path']);value=update._load(path)
        for changed in (dict(value,schema=update.REQUEST_SCHEMA),dict(value,installer=dict(value['installer'],status='verified'))):
            path.unlink();update._write(path,{k:v for k,v in changed.items() if k!='record_sha256'})
            with self.assertRaises(update.AppUpdateError):update._request(path,request['nonce'])
        self.preserved()

    def test_only_actual_requesting_candidate_may_launch_helper(self):
        request=self.stage();self.processes.current=self.processes.old
        with self.assertRaisesRegex(update.AppUpdateError,'Only the requesting'):update.launch_updater(request,processes=self.processes)
        self.assertEqual(self.processes.spawned,[]);self.preserved()

    def test_candidate_mutation_before_helper_launch_refuses(self):
        request=self.stage();(self.caller/'_internal/runtime.dll').write_bytes(b'changed')
        with self.assertRaisesRegex(update.AppUpdateError,'checksum'):update.launch_updater(request,processes=self.processes)
        self.assertEqual(self.processes.spawned,[]);self.preserved()

    def test_matching_helper_ready_uses_real_bootstrap_identity(self):
        request=self.stage();path=Path(request['request_path']);pin=update._file(path,update.MAX_JSON)
        def spawn(helper,args):update._receipt(path.parent,'helper-ready.json',pin,request['nonce'],helper=helper)
        self.processes.on_spawn=spawn
        ready=update.launch_updater(request,processes=self.processes)
        self.assertEqual(ready['status'],'helper_ready')
        self.assertEqual(ready['nonce'],request['nonce'])
        self.assertEqual(update._load(path)['parent'],self.bootstrap)
        self.assertEqual(self.processes.spawned[0][1][0],'--apply-app-update');self.preserved()

    def test_full_local_apply_waits_candidate_exit_and_commits_real_child_ack(self):
        request=self.stage();self.assertEqual(self.apply(request),0)
        result=update._load(Path(request['request_path']).parent/'result.json')
        self.assertEqual(result['status'],'committed')
        self.assertNotIn(111,self.processes.living)
        self.assertTrue(any(x[0]=='wait' for x in self.processes.events))
        self.assertEqual(update._bundle(self.target,'1.3.6'),update._bundle(self.caller,'1.3.6'))
        self.assertEqual(self.processes.spawned[-1][1][0],'--app-update-ack');self.preserved()

    def test_candidate_directory_can_retire_after_verified_parent_exit(self):
        request=self.stage();original_parent=self.processes.parent
        @contextmanager
        def retained_parent(identity):
            with original_parent(identity) as wait:
                def wait_then_retire(seconds):
                    closed=wait(seconds)
                    if closed:
                        self.assertTrue(self.caller.resolve().is_relative_to(self.root.resolve()))
                        self.caller.rename(self.root/'retired-candidate')
                    return closed
                yield wait_then_retire
        self.processes.parent=retained_parent
        self.assertEqual(self.apply(request),0)
        self.assertFalse(self.caller.exists())
        self.assertEqual(update._bundle(self.target,'1.3.6'),update._bundle(self.root/'retired-candidate','1.3.6'))
        self.preserved()

    def test_candidate_not_exited_refuses_without_moving_old_app(self):
        request=self.stage();self.processes.wait_success=False
        self.assertEqual(self.apply(request),2)
        self.assertEqual((self.target/'TextureQuest.exe').read_bytes(),self.old['TextureQuest.exe'])
        self.assertFalse((Path(request['request_path']).parent/'backup').exists());self.preserved()

    def test_child_ack_failure_restores_old_bundle_with_local_failure_notice(self):
        request=self.stage();self.processes.auto_ack=False;self.processes.alert=MagicMock()
        self.assertEqual(self.apply(request),2)
        result=update._load(Path(request['request_path']).parent/'result.json')
        self.assertEqual(result['rollback'],'restored')
        self.assertEqual((self.target/'TextureQuest.exe').read_bytes(),self.old['TextureQuest.exe'])
        self.assertEqual(self.processes.spawned[-1][1][0],'--app-update-failed')
        self.processes.alert.assert_called_once();self.assertIn('saved data was not changed',self.processes.alert.call_args.args[0])
        self.preserved()


class LocalBuildCLI(unittest.TestCase):
    def test_packaged_cli_opens_local_gui_without_normal_app_or_confirmation(self):
        root=Root()
        with patch.object(app.sys,'frozen',True,create=True),patch.object(app.tk,'Tk',return_value=root),\
             patch.object(app,'LocalBuildInstallUI') as local,patch.object(app,'TextureQuestApp') as normal,\
             patch.object(app.messagebox,'askyesno') as ask:
            self.assertEqual(app.main(['--install-build','D:/invented/candidate.zip','--install-dir','D:/invented/installed']),0)
        local.assert_called_once_with(root,Path('D:/invented/candidate.zip'),Path('D:/invented/installed'))
        normal.assert_not_called();ask.assert_not_called()

    def test_cli_rejects_custom_data_other_operations_missing_target_and_source_mode(self):
        with patch.object(app.sys,'frozen',True,create=True),patch.object(app.tk,'Tk') as create,redirect_stderr(StringIO()):
            for args in (['--install-build','D:/a.zip'],['--install-dir','D:/app'],
                         ['--install-build','D:/a.zip','--install-dir','D:/app','--data-dir','D:/custom'],
                         ['--install-build','D:/a.zip','--install-dir','D:/app','--smoke-test'],
                         ['--install-build','D:/a.zip','--apply-app-update','D:/request']):
                with self.subTest(args=args),self.assertRaises(SystemExit):app.main(args)
            create.assert_not_called()
        with patch.object(app.sys,'frozen',False,create=True),patch.object(app.tk,'Tk') as create,redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit):app.main(['--install-build','D:/a.zip','--install-dir','D:/app'])
            create.assert_not_called()

    def fake(self):
        ui=app.LocalBuildInstallUI.__new__(app.LocalBuildInstallUI)
        ui.root=Root();ui.archive=Path('D:/invented/candidate.zip');ui.install_dir=Path('D:/invented/installed')
        ui.events=queue.Queue();ui.started=ui.busy=ui.closing=False
        ui.status=Widget();ui.progress=Widget();ui.close_button=Widget()
        return ui

    def test_local_gui_is_async_and_closes_only_after_matching_ready(self):
        ui=self.fake();work=[]
        request=dict(request_path='D:/owned/request.json',nonce='b'*32,target_version='1.3.6',app_dir=str(ui.install_dir),data_dir=str(ui.install_dir/'data'))
        backend=types.SimpleNamespace(stage_local_build_install=MagicMock(return_value=request),launch_updater=MagicMock(return_value=dict(request,status='helper_ready')))
        def thread(*,target,daemon):work.append(target);return types.SimpleNamespace(start=lambda:None)
        with patch.dict(sys.modules,{'texturequest_self_update':backend}),patch.object(app.threading,'Thread',side_effect=thread):
            ui.start();ui.start();self.assertEqual(len(work),1)
            backend.stage_local_build_install.assert_not_called();self.assertFalse(ui.root.destroyed)
            ui.close();self.assertFalse(ui.root.destroyed)
            work[0]();self.assertFalse(ui.root.destroyed);ui.poll()
        self.assertTrue(ui.root.destroyed);self.assertTrue(ui.closing)
        self.assertEqual(backend.stage_local_build_install.call_args.kwargs['running_version'],app.APP_VERSION)

    def test_local_gui_failure_or_foreign_ready_stays_open_without_touching_normal_app(self):
        for failure in ('preparation','foreign-ready'):
            ui=self.fake();work=[]
            request=dict(request_path='D:/owned/request.json',nonce='b'*32)
            backend=types.SimpleNamespace(stage_local_build_install=MagicMock(return_value=request),launch_updater=MagicMock(return_value=dict(request,status='helper_ready',nonce='c'*32)))
            if failure=='preparation':backend.stage_local_build_install.side_effect=update.AppUpdateError('candidate mismatch')
            def thread(*,target,daemon):work.append(target);return types.SimpleNamespace(start=lambda:None)
            with patch.dict(sys.modules,{'texturequest_self_update':backend}),patch.object(app.threading,'Thread',side_effect=thread):
                ui.start();work[0]();ui.poll()
            self.assertFalse(ui.root.destroyed);self.assertFalse(ui.busy);self.assertIn('stopped',ui.status.cget('text'))
            if failure=='preparation':backend.launch_updater.assert_not_called()


if __name__=='__main__':unittest.main()
