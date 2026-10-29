"""New update-button/prerequisite cases with invented D files and mocked leaves."""
from unittest.mock import patch
from test_background_loader_ui import BackgroundLoaderUI, module
import texturequest_self_update as updates

class UpdateLoaderPrerequisiteUI(BackgroundLoaderUI):
    def ready(self):
        self.app.update_check = {'installer': dict(status='verified', update_available=True, version='9.0.0')}
        self.state.update(installed=True, running=True, active_sessions=0)
        self.app.refresh_controls()

    def test_available_button_and_confirmed_uninstall_precede_update_without_cache_writes(self):
        self.ready()
        self.assertEqual(self.app.installer_update_button.cget('state'), 'normal')
        order=[]
        def uninstall(game):
            self.assertEqual(game,str(self.game))
            order.append('uninstall')
            self.state.update(installed=False,running=False)
        self.manager.uninstall.side_effect=uninstall
        request=dict(request_path='invented-request',nonce='a'*32)
        def stage(*args,**kwargs):
            order.append('stage'); return request
        with patch.object(module.sys,'frozen',True,create=True), \
             patch.object(module.messagebox,'askyesno',return_value=True) as ask, \
             patch.object(updates,'stage_update',side_effect=stage), \
             patch.object(updates,'launch_updater',return_value=dict(request,status='helper_ready')):
            self.assertTrue(self.app.open_app_update())
            self.assertIn('Install background loader again',ask.call_args.args[1])
            self.assertEqual(self.run_action()['status'],'helper_ready')
        self.assertEqual(order,['uninstall','stage'])
        self.manager.install.assert_not_called()
        self.assertNotIn('A/B',module.INSTALL_MODE_INFO[1][3])

    def test_cancel_preserves_loader_and_app(self):
        self.ready()
        with patch.object(module.sys,'frozen',True,create=True), \
             patch.object(module.messagebox,'askyesno',return_value=False):
            self.assertFalse(self.app.open_app_update())
        self.manager.uninstall.assert_not_called();self.begin.assert_not_called()

    def test_active_or_unknown_loader_is_explained_without_uninstall(self):
        self.ready()
        for broken in (False, True):
            self.state.update(active_sessions=0 if broken else 1)
            self.manager.status.side_effect = ValueError('invented unreadable ownership') if broken else lambda game:dict(self.state)
            with patch.object(module.sys,'frozen',True,create=True), \
                 patch.object(module.messagebox,'askyesno') as ask:
                self.assertFalse(self.app.open_app_update());ask.assert_not_called()
                self.assertIn('Close loaded EQ2 sessions',self.message())
        self.manager.uninstall.assert_not_called();self.begin.assert_not_called()

    def test_worker_rechecks_cache_after_uninstall_and_preserves_app_on_changed_state(self):
        self.ready()
        with patch.object(module.sys,'frozen',True,create=True), \
             patch.object(module.messagebox,'askyesno',return_value=True), \
             patch.object(updates,'stage_update') as stage:
            self.assertTrue(self.app.open_app_update())
            with self.assertRaisesRegex(RuntimeError,'background loader'):
                self.run_action()
            stage.assert_not_called()
        self.manager.uninstall.assert_called_once()

    def test_busy_still_disables_update_and_unverified_update_never_uninstalls(self):
        self.ready();self.app.busy='app-update';self.app.refresh_controls()
        self.assertEqual(self.app.installer_update_button.cget('state'),'disabled')
        self.app.busy=None;self.app.update_check={'installer':dict(status='unavailable')}
        with patch.object(module.messagebox,'askyesno') as ask:
            self.assertFalse(self.app.open_app_update());ask.assert_not_called()
        self.manager.uninstall.assert_not_called()
