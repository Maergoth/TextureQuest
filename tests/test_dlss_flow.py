"""Headless normal-flow contracts; no Tk window, network, game or native calls."""
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import texturequest_app as app
import texturequest_setup as setup
import texturequest_components as components


class Variable:
    def __init__(self, value): self.value = value
    def get(self): return self.value
    def set(self, value): self.value = value


class DLSSFlow(unittest.TestCase):
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / 'build/dlss-flow-fixtures-001'
        base.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=base))
        self.game = self.root / 'unopened game ü'
        self.selection = dict(components.normalize(), dlss=True)
        self.status = dict(installed=True, running=False, active_sessions=0, available=True, unknown=False,
                           components=components.normalize(), installation_revision='own-record',
                           effects_available=True, dlss_available=True, message='ready')
        ui = types.SimpleNamespace(state_dir=self.root / 'data', game=Variable(str(self.game)), busy=None,
            component_effects=Variable(False), component_dlss=Variable(False), component_error='', component_snapshot=None,
            prepared={}, release_checked=True, graphics_status=Mock(side_effect=lambda *_: dict(self.status)),
            graphics_manager=Mock(), loader_status=Mock(return_value={}), loader_in_use=Mock(return_value=False),
            upgrade_pending=Mock(return_value=False), graphics_selected=Mock(return_value=True),
            set_message=Mock(), refresh_controls=Mock(), _download_latest=Mock(), dds_store=Mock(),
            report=Mock(), shortcut={'vk': 106, 'modifiers': 0}, upgrade=Mock())
        for name in ('selected_components', 'load_components', 'components_changed'):
            setattr(ui, name, types.MethodType(getattr(app.TextureQuestApp, name), ui))
        ui.begin = Mock(side_effect=lambda kind, message, action: action())
        self.ui = ui

    def test_dlss_choice_saves_loads_and_stays_separate_from_installed_capability(self):
        self.ui.load_components()
        self.ui.component_dlss.set(True); self.ui.components_changed()
        self.assertTrue(components.load(self.ui.state_dir, self.game)['selection']['dlss'])
        self.assertFalse(self.status['components']['dlss'])
        self.ui.component_dlss.set(False); self.ui.load_components()
        self.assertTrue(self.ui.component_dlss.get())
        self.ui.game.set(str(self.root / 'other unopened game')); self.ui.load_components()
        self.assertFalse(self.ui.component_dlss.get())
        self.assertFalse(self.game.exists())

    def test_unavailable_choice_refuses_and_restores_saved_preference(self):
        self.ui.load_components(); self.status['dlss_available'] = False
        self.ui.component_dlss.set(True); self.ui.components_changed()
        self.assertFalse(self.ui.component_dlss.get())
        self.assertFalse(components.load(self.ui.state_dir, self.game)['selection']['dlss'])
        self.assertIn('complete experimental DLSS profile', self.ui.set_message.call_args.args[0])

    def test_checkbox_is_enabled_only_for_verified_profile_when_idle(self):
        ui = self.ui
        ui.controls, ui.mode_controls = [], []
        for name in ('game_entry', 'shortcut_button', 'install_button', 'menu', 'launch_button', 'version_label',
                     'app_update_label', 'installer_update_button', 'pack_description', 'pack_update_label',
                     'pack_versions_label', 'download_button', 'download_label', 'install_label'):
            setattr(ui, name, Mock())
        ui.component_controls = {key: Mock() for key in ('textures', 'dlss', 'effects')}
        ui.button_state = Mock(); ui.legacy_uninstall_index = 1
        ui.app_version, ui.update_check, ui.dds_source, ui.release = '1.10.3', {}, {}, {}
        with patch.object(app, 'game_executable', return_value=self.game / 'EverQuest2.exe'):
            for available, busy, error, expected in ((True, None, '', 'normal'), (False, None, '', 'disabled'),
                    (True, 'install', '', 'disabled'), (True, None, 'bad saved selection', 'disabled')):
                self.status['dlss_available'], ui.busy, ui.component_error = available, busy, error
                app.TextureQuestApp.refresh_controls(ui)
                self.assertEqual(ui.component_controls['dlss'].configure.call_args.kwargs['state'], expected)
                self.assertEqual(ui.component_controls['textures'].configure.call_args.kwargs['state'], 'disabled')
            ui.busy, ui.component_error = None, ''
            self.status['dlss_available'] = False; ui.component_dlss.set(True)
            app.TextureQuestApp.refresh_controls(ui)
            self.assertEqual(ui.component_controls['dlss'].configure.call_args.kwargs['state'], 'normal')
            ui.component_dlss.set(False); ui.components_changed()
            self.assertFalse(components.load(ui.state_dir, self.game)['selection']['dlss'])

    def test_apply_changes_preflight_refusal_keeps_working_hook(self):
        self.ui.component_dlss.set(True)
        self.ui.prepared = {'distribution': 'generated-channels-only'}
        self.ui.graphics_manager.preflight_components.side_effect = ValueError('selected dependency changed')
        with self.assertRaisesRegex(ValueError, 'dependency changed'):
            app.TextureQuestApp.manage_graphics(self.ui)
        self.ui.graphics_manager.uninstall.assert_not_called()
        self.ui.graphics_manager.install.assert_not_called()

    def setup_flow(self, failure=False):
        order = []
        state = self.root / 'setup-data'; state.mkdir()
        prepared = dict(distribution='generated-channels-only')
        store = Mock(); store.prepare.return_value = prepared
        store.verify.side_effect = lambda *a, **kw: order.append('verified-textures')
        graphics = Mock(); graphics.status.return_value = dict(self.status)
        if failure: graphics.preflight_components.side_effect = ValueError('optional profile changed')
        else: graphics.preflight_components.side_effect = lambda *a: order.append('verified-dlss')
        graphics.install.side_effect = lambda *a, **kw: order.append('install')
        upgrade = Mock(state=state); upgrade.needed.return_value = True
        upgrade.before.side_effect = lambda *a, **kw: order.append('remove-old')
        upgrade.complete.side_effect = lambda *a, **kw: order.append('complete')
        with patch.object(setup, 'ensure_closed'), patch.object(setup.core, '_load', return_value=prepared):
            args = (self.game, Mock(return_value={}), {}, store, graphics, upgrade)
            kwargs = dict(minimum='1.3.0', binding={'vk':106, 'modifiers':0}, in_use=lambda:False,
                          auto_install=True, components=self.selection)
            if failure:
                with self.assertRaisesRegex(ValueError, 'optional profile changed'):
                    setup.prepare_graphics(*args, **kwargs)
            else: setup.prepare_graphics(*args, **kwargs)
        return order, graphics, upgrade

    def test_automatic_upgrade_preflight_precedes_retirement(self):
        order, _, _ = self.setup_flow()
        self.assertEqual(order, ['verified-textures', 'verified-dlss', 'remove-old', 'install', 'complete'])

    def test_automatic_upgrade_optional_failure_never_removes_hook(self):
        order, graphics, upgrade = self.setup_flow(failure=True)
        self.assertEqual(order, ['verified-textures'])
        upgrade.before.assert_not_called(); graphics.install.assert_not_called()


if __name__ == '__main__': unittest.main()
