"""New recovery cases: invented D-only files, fake network/process/UI leaves."""
from contextlib import ExitStack, contextmanager
import copy
import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

import texturequest_app as app
import texturequest_self_update as updater
import texturequest_setup as setup
import texturequest_upgrade as upgrade
import texturequest_graphics_proxy as graphics
from texturequest_release import Paused
from texture_swapper import core
from test_app_self_update_ui import fake_app
from test_app_self_update_backend import bundle, make_bundle, make_zip
from test_graphics_proxy import Fixture


def installer(version, digest='a'*64, size=120, tag=None):
    tag = tag or version
    return dict(status='verified', version=version, release_url=updater.latest.REPOSITORY+'/releases/tag/v'+tag,
        download_url=updater.latest.REPOSITORY+'/releases/download/v'+tag+'/TextureQuest-Installer-'+version+'-windows-x64.zip',
        bytes=size, sha256=digest)


def metadata(value):
    return dict(tag_name=value['release_url'].rsplit('/',1)[-1], html_url=value['release_url'],
        assets=[dict(name=value['download_url'].rsplit('/',1)[-1], state='uploaded',
                     browser_download_url=value['download_url'],size=value['bytes'],digest='sha256:'+value['sha256'])])


class IdentityRecovery(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.app=self.root/'TextureQuest';self.app.mkdir();self.data=self.app/'data';self.data.mkdir()
        self.marker=self.data/'textures.marker';self.marker.write_bytes(b'keep custom files')
        self.selected=installer('1.9.5');self.progress=[]

    def stage(self,fresh, **extra):
        with patch.object(updater.versions,'_metadata',return_value=metadata(fresh)), \
             patch.object(updater.versions,'_version_asset',return_value=None), \
             patch.object(updater,'_download',return_value=None) as download, \
             patch.object(updater,'stage_downloaded_update',return_value={'target_version':fresh['version']}) as staged:
            result=updater.stage_update(self.selected,app_dir=self.app,data_dir=self.data,current_version='1.9.4',report=self.progress.append)
            return result,download,staged

    def test_genuinely_newer_official_target_refreshes_once(self):
        fresh=installer('1.9.6','b'*64,121)
        result,download,staged=self.stage(fresh)
        self.assertEqual(result['target_version'],'1.9.6');self.assertEqual(download.call_args.args[0],fresh)
        staged.assert_called_once();self.assertTrue(any('1.9.6' in x['message'] and x['phase']=='refresh' for x in self.progress))
        receipt=updater._load(next((self.data/'app-update-checks').glob('*.json')))
        self.assertEqual(receipt['decision'],'newer-official-target');self.assertEqual(receipt['selected'],self.selected)
        self.assertEqual(self.marker.read_bytes(),b'keep custom files')

    def refusal(self,fresh,field):
        with patch.object(updater.versions,'_metadata',return_value=metadata(fresh)), \
             patch.object(updater.versions,'_version_asset',return_value=None),patch.object(updater,'_download') as download:
            with self.assertRaisesRegex(updater.AppUpdateError,field):
                updater.stage_update(self.selected,app_dir=self.app,data_dir=self.data,current_version='1.9.4')
        download.assert_not_called();self.assertEqual(self.marker.read_bytes(),b'keep custom files')
        receipt=updater._load(next((self.data/'app-update-checks').glob('*.json')))
        self.assertEqual(receipt['decision'],'refused');self.assertIn(field,receipt['differing_fields'])

    def test_same_version_digest_mutation_refuses_with_safe_detail(self):
        self.refusal(installer('1.9.5','c'*64),'sha256')

    def test_same_version_size_mutation_refuses(self):
        self.refusal(installer('1.9.5',size=121),'bytes')

    def test_same_version_release_envelope_mutation_refuses(self):
        self.refusal(installer('1.9.5',tag='1.9.6'),'release_url')

    def test_target_downgrade_refuses_before_download(self):
        self.refusal(installer('1.9.3'),'version')

    def test_unchanged_verified_identity_has_no_refresh_record(self):
        self.stage(self.selected)
        self.assertFalse((self.data/'app-update-checks').exists());self.assertEqual(self.progress,[])

    def test_metadata_failure_leaves_data_and_no_download(self):
        with patch.object(updater.versions,'_metadata',side_effect=OSError('invented metadata failure')),patch.object(updater,'_download') as download:
            with self.assertRaisesRegex(OSError,'invented'):
                updater.stage_update(self.selected,app_dir=self.app,data_dir=self.data,current_version='1.9.4')
        download.assert_not_called();self.assertEqual(self.marker.read_bytes(),b'keep custom files')

    def test_refreshed_target_still_uses_complete_zip_bundle_admission(self):
        old=bundle('1.9.4',b'O')
        for name,raw in old.items():
            path=self.app/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(raw)
        archive=self.root/'new.zip';make_zip(archive,bundle('1.9.6',b'N'));raw=archive.read_bytes()
        fresh=installer('1.9.6',hashlib.sha256(raw).hexdigest(),len(raw))
        processes=MagicMock();processes.identity.return_value=dict(pid=100,birth=100.25,exe=str(self.app/'TextureQuest.exe'))
        processes.alive.return_value=True
        def download(value,destination,*args):
            self.assertEqual(value,fresh);destination.write_bytes(raw);return updater._file(destination,updater.MAX_ZIP)
        with patch.object(updater.versions,'_metadata',return_value=metadata(fresh)),patch.object(updater.versions,'_version_asset',return_value=None), \
             patch.object(updater,'_download',side_effect=download),patch.object(updater,'WindowsProcesses',return_value=processes):
            request=updater.stage_update(self.selected,app_dir=self.app,data_dir=self.data,current_version='1.9.4')
        value=updater._load(Path(request['request_path']))
        self.assertEqual(value['target_version'],'1.9.6');self.assertEqual(value['installer'],fresh)
        self.assertEqual(updater._bundle(self.app,'1.9.4'),value['old_bundle'])
        self.assertEqual(self.marker.read_bytes(),b'keep custom files')


class SetupOrder(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.state=Path(self.temp.name);self.marker=self.state/'working-hook.marker';self.marker.write_bytes(b'working')
        self.order=[];self.paused=False;self.installed=True
        self.prepared=dict(distribution='generated-channels-only',texture_version='1.3.0')
        core._save(self.state/'dds-prepared.json',self.prepared)
        self.store=MagicMock();self.graphics=MagicMock();self.migration=MagicMock(state=self.state)
        self.graphics.status.side_effect=lambda game:dict(installed=self.installed,installed_newer_app=False)
        self.migration.needed.return_value=True;self.migration.snapshot.return_value={'graphics':{'id':'old'}}
        self.store.prepare.side_effect=lambda *a,**kw:self.mark('prepare',self.prepared)
        self.store.verify.side_effect=lambda *a,**kw:self.mark('FULLverify',a[0])
        self.migration.before.side_effect=self.retire
        self.graphics.activate_prepared.side_effect=lambda *a,**kw:self.mark('activate')
        self.graphics.install.side_effect=lambda *a,**kw:self.mark('install')
        self.migration.complete.side_effect=lambda *a,**kw:self.mark('cleanup')
        self.closed=patch.object(setup,'ensure_closed');self.closed.start();self.addCleanup(self.closed.stop)

    def mark(self,name,value=None):self.order.append(name);return value
    def retire(self,*a,**kw):self.order.append('retire');self.installed=False
    def run_setup(self,**changes):
        args=dict(minimum='1.3.0',binding={'vk':106,'modifiers':0},in_use=lambda:False,auto_install=True,pause=lambda:self.paused)
        args.update(changes)
        return setup.prepare_graphics('invented game',lambda **kw:self.mark('fetch',{'source':'verified fixture'}),{},self.store,self.graphics,self.migration,**args)
    def kept(self):
        self.migration.before.assert_not_called();self.graphics.install.assert_not_called();self.graphics.activate_prepared.assert_not_called()
        self.assertEqual(self.marker.read_bytes(),b'working')

    def test_full_preparation_precedes_retirement_install_and_cleanup(self):
        self.run_setup();self.assertEqual(self.order,['fetch','prepare','FULLverify','retire','install','cleanup'])
        self.store.verify.assert_called_once_with(self.prepared,full=True)
        self.assertEqual(self.migration.before.call_args.kwargs['snapshot'],{'graphics':{'id':'old'}})

    def test_failed_preparation_preserves_working_hook(self):
        self.store.prepare.side_effect=OSError('invented preparation failure')
        with self.assertRaises(OSError):self.run_setup()
        self.kept()

    def test_failed_full_verification_preserves_working_hook(self):
        self.store.verify.side_effect=ValueError('invented checksum failure')
        with self.assertRaisesRegex(ValueError,'checksum'):self.run_setup()
        self.kept()

    def test_cancel_after_preparation_preserves_working_hook(self):
        def prepare(*a,**kw):self.paused=True;return self.prepared
        self.store.prepare.side_effect=prepare
        with self.assertRaises(Paused):self.run_setup()
        self.kept()

    def test_changed_prepared_receipt_refuses_retirement(self):
        core._save(self.state/'dds-prepared.json',dict(self.prepared,texture_version='1.3.1'))
        with self.assertRaisesRegex(ValueError,'preparation changed'):self.run_setup()
        self.kept()

    def test_newer_hook_refuses_before_source_fetch(self):
        self.graphics.status.return_value=None;self.graphics.status.side_effect=lambda game:dict(installed=True,installed_newer_app=True)
        with self.assertRaisesRegex(ValueError,'newer'):self.run_setup()
        self.assertEqual(self.order,[]);self.kept()

    def test_real_in_use_guard_is_not_bypassed(self):
        with self.assertRaisesRegex(ValueError,'Close EQ2'):self.run_setup(in_use=lambda:True)
        self.store.verify.assert_not_called();self.kept()

    def test_cancel_during_full_verify_stops_before_retirement(self):
        self.store.verify.side_effect=lambda *a,**kw:setattr(self,'paused',True)
        with self.assertRaises(Paused):self.run_setup()
        self.kept()

    def test_concurrent_install_after_retirement_cannot_redirect_its_map(self):
        def race(*a,**kw):self.order.append('retire');self.installed=True
        self.migration.before.side_effect=race
        self.graphics.install.side_effect=ValueError('Concurrent installed hook admission refused')
        with self.assertRaisesRegex(ValueError,'Concurrent installed'):
            self.run_setup()
        self.graphics.install.assert_called_once();self.graphics.activate_prepared.assert_not_called()
        self.migration.complete.assert_not_called()


class ReceiptCAS(Fixture):
    def test_installed_modern_addon_without_game_is_not_a_download_lock(self):
        self.install()
        self.assertTrue(self.manager.cache_in_use())
        self.assertFalse(self.manager.downloads_in_use())

    def test_changed_receipt_refuses_inside_normal_uninstall_locks(self):
        self.install();before=self.manager._record(self.game);changed=dict(before,id='f'*32);self.manager._save(self.game,changed)
        files={r['path']:Path(r['path']).read_bytes() for r in before['files']}
        held=set();real_lock=core._lock;read=self.manager._record
        @contextmanager
        def lock(path):
            with real_lock(path):
                held.add(Path(path));yield;held.remove(Path(path))
        def record(game):
            self.assertIn(self.state/'operation.lock',held);self.assertIn(self.state/'sideload/helper-owner.lock',held)
            return read(game)
        with patch.object(core,'_lock',side_effect=lock),patch.object(self.manager,'_record',side_effect=record):
            with self.assertRaisesRegex(ValueError,'record changed'):self.manager.uninstall(self.game,expected_record=before)
        self.assertEqual({p:Path(p).read_bytes() for p in files},files);self.assertEqual(read(self.game)['phase'],'installed')

    def test_snapshot_retains_old_preparation_for_cleanup_history(self):
        self.install();migration=upgrade.Upgrade(self.state,self.manager)
        old=dict(distribution='legacy-full-dds',texture_version='1.2.3',note='retained old identity')
        core._save(self.state/'dds-prepared.json',old)
        snapshot=migration.snapshot(self.game)
        core._save(self.state/'dds-prepared.json',dict(distribution='generated-channels-only',texture_version='1.3.0'))
        with patch.object(upgrade,'ensure_closed'),patch.object(migration,'_stock'):
            migration.before(self.game,snapshot=snapshot)
        self.assertEqual(core._load(migration.receipt(self.game))['previous_dds'],old)
        self.assertEqual(self.manager._record(self.game)['phase'],'uninstalled')

    def test_changed_snapshot_preserves_hook_before_migration_receipt(self):
        self.install();migration=upgrade.Upgrade(self.state,self.manager);snapshot=migration.snapshot(self.game)
        changed=self.manager._record(self.game);changed['id']='e'*32;self.manager._save(self.game,changed)
        with patch.object(upgrade,'ensure_closed'),self.assertRaisesRegex(ValueError,'record changed'):
            migration.before(self.game,snapshot=snapshot)
        self.assertFalse(migration.receipt(self.game).exists());self.assertEqual(self.manager._record(self.game)['phase'],'installed')

    def test_newer_hook_after_snapshot_refuses_before_any_retirement(self):
        self.install();migration=upgrade.Upgrade(self.state,self.manager);snapshot=migration.snapshot(self.game)
        changed=self.manager._record(self.game);changed['app_version']='9.0.0';self.manager._save(self.game,changed)
        with patch.object(upgrade,'ensure_closed'),self.assertRaisesRegex(ValueError,'newer TextureQuest'):
            migration.before(self.game,snapshot=snapshot)
        self.assertFalse(migration.receipt(self.game).exists());self.assertEqual(self.manager._record(self.game)['phase'],'installed')

    def test_actual_small_channel_cache_prepares_and_verifies_before_refresh(self):
        from test_channels import fixture
        from texturequest_channels import ChannelStore
        game,source,index,examples,choices,opener,requests=fixture(self.root/'channels',(4,))
        self.game=game;self.install()
        old=self.manager._record(game);old['app_version']='1.9.3';self.manager._save(game,old)
        store=ChannelStore(self.state);migration=upgrade.Upgrade(self.state,self.manager)
        prepare=store.prepare
        with patch.object(setup,'ensure_closed'),patch.object(upgrade,'ensure_closed'), \
             patch.object(store,'prepare',side_effect=lambda *a,**kw:prepare(*a,**kw,opener=opener)):
            _,prepared=setup.prepare_graphics(game,lambda **kw:source,choices,store,self.manager,migration,
                minimum='1.3.0',binding={'vk':106,'modifiers':0},in_use=self.manager.downloads_in_use,auto_install=True)
        self.assertEqual(prepared['distribution'],'generated-channels-only')
        self.assertEqual(self.manager._record(game)['app_version'],app.APP_VERSION)
        self.assertNotEqual(self.manager._record(game)['id'],old['id'])
        self.assertEqual(core._load(migration.receipt(game))['status'],'complete')


class PassiveAndAppOrder(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        self.ui=fake_app(self.root);self.ui.refresh_controls=lambda:None;self.ui.initial_pack=None
        self.ui.game=SimpleNamespace(get=lambda:str(self.root/'invented game'))

    def version_result(self,newer):
        return dict(installer=dict(status='verified',update_available=newer,version='1.9.5'),
            pack=dict(status='verified',manifest=self.ui.release,version='1.2.3'))

    def test_passive_check_with_available_app_never_starts_migration(self):
        self.ui._download_latest=MagicMock();self.ui.verify_installation=MagicMock();self.ui.upgrade=MagicMock()
        self.ui.busy='check';self.ui.events.put(('done','check',self.version_result(True)));self.ui.poll()
        self.ui._download_latest.assert_not_called();self.ui.verify_installation.assert_not_called()
        self.assertIn('Get app update first',self.ui.messages[-1][0])

    def test_passive_equal_version_check_never_starts_migration(self):
        self.ui._download_latest=MagicMock();self.ui.upgrade=MagicMock();self.ui.busy='check'
        self.ui.events.put(('done','check',self.version_result(False)));self.ui.poll();self.ui._download_latest.assert_not_called()

    def test_explicit_texture_setup_defers_to_available_app(self):
        self.ui.begin=MagicMock()
        self.assertFalse(self.ui._download_latest(auto_install=True,upgrading=True));self.ui.begin.assert_not_called()
        self.assertIn('Get app update',self.ui.messages[-1][0])

    def legacy_update(self,stage_failure=False):
        order=[];self.ui.graphics_manager=MagicMock();self.ui.loader_manager=MagicMock()
        self.ui.cache_in_use=MagicMock(side_effect=[True,False]);self.ui.graphics_manager.legacy_in_use.return_value=True
        self.ui.graphics_status=MagicMock(return_value=dict(unknown=False,installed=True,active_sessions=0))
        self.ui.graphics_manager.uninstall.side_effect=lambda *a,**kw:order.append('uninstall')
        request=dict(request_path=str(self.root/'request.json'),nonce='a'*32,target_version='1.9.5')
        backend=SimpleNamespace(stage_update=MagicMock(side_effect=OSError('stage failure') if stage_failure else lambda *a,**kw:order.append('staged') or request),
            launch_updater=MagicMock(side_effect=lambda value:order.append('ready') or dict(value,status='helper_ready')))
        self.ui.begin=lambda kind,message,action:action()
        with patch.object(app.sys,'frozen',True,create=True),patch.object(app.sys,'executable',str(self.root/'TextureQuest.exe')), \
             patch.dict('sys.modules',{'texturequest_self_update':backend}),patch.object(app.messagebox,'askyesno',return_value=True):
            if stage_failure:
                with self.assertRaises(OSError):self.ui.open_app_update()
                self.ui.graphics_manager.uninstall.assert_not_called()
            else:self.ui.open_app_update();self.assertEqual(order,['staged','uninstall','ready'])

    def test_app_update_staging_failure_never_retires_legacy_hook(self):self.legacy_update(True)
    def test_app_update_stages_before_legacy_hook_retirement(self):self.legacy_update(False)

    def test_authenticated_app_startup_arms_only_one_post_update_setup(self):
        self.ui.app_version='1.9.5';self.ui.check_updates=MagicMock();self.ui._download_latest=MagicMock()
        backend=SimpleNamespace(acknowledge_startup=MagicMock(),failure_message=MagicMock())
        with patch.dict('sys.modules',{'texturequest_self_update':backend}):
            app.finish_app_update_startup(self.ui,str(self.root/'request.json'),'a'*32)
        backend.acknowledge_startup.assert_called_once();self.assertTrue(self.ui.post_update_setup)
        self.ui.busy='check';self.ui.events.put(('done','check',self.version_result(False)));self.ui.poll()
        self.ui._download_latest.assert_called_once_with(auto_install=True,upgrading=True);self.assertFalse(self.ui.post_update_setup)
        self.ui.busy='check';self.ui.events.put(('done','check',self.version_result(False)));self.ui.poll()
        self.ui._download_latest.assert_called_once()

    def test_failed_startup_ack_never_arms_post_update_setup(self):
        backend=SimpleNamespace(acknowledge_startup=MagicMock(side_effect=ValueError('ACK failed')),failure_message=MagicMock())
        with patch.dict('sys.modules',{'texturequest_self_update':backend}):
            app.finish_app_update_startup(self.ui,str(self.root/'request.json'),'a'*32)
        self.assertFalse(getattr(self.ui,'post_update_setup',False))

    def test_explicit_setup_progress_names_preparation_and_full_verification(self):
        with patch.object(app.threading,'Thread') as thread:
            self.ui.begin('update','Preparing replacement textures',lambda:None)
        thread.return_value.start.assert_called_once()
        self.assertIn('Texture setup: preparing',self.ui.overall_label.cget('text'))
        self.ui.events.put(('overall-progress','Checking the texture release',0,0,0,0));self.ui.poll()
        self.assertIn('Checking the texture release',self.ui.overall_label.cget('text'))
        self.ui.events.put(('overall-progress','Verifying replacement textures',0,0,0,0));self.ui.poll()
        self.assertIn('Verifying replacement textures',self.ui.overall_label.cget('text'))
        self.assertIn('installed hook kept until ready',self.ui.overall_label.cget('text'))


if __name__=='__main__':unittest.main()
