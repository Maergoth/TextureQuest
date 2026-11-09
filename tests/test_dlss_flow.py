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
            component_effects=Variable(False), component_dlss=Variable(False), component_graphics=Variable(False), component_error='', component_snapshot=None,
            prepared={}, release_checked=True, graphics_status=Mock(side_effect=lambda *_: dict(self.status)),
            graphics_manager=Mock(), loader_status=Mock(return_value={}), loader_in_use=Mock(return_value=False),
            upgrade_pending=Mock(return_value=False), graphics_selected=Mock(return_value=True),
            set_message=Mock(), refresh_controls=Mock(), _download_latest=Mock(), dds_store=Mock(),
            report=Mock(), shortcut={'vk': 106, 'modifiers': 0}, upgrade=Mock())
        for name in ('selected_components', 'save_components', 'load_components', 'components_changed'):
            setattr(ui, name, types.MethodType(getattr(app.TextureQuestApp, name), ui))
        ui.begin = Mock(side_effect=lambda kind, message, action: action())
        self.ui = ui

    def test_umbrella_choice_saves_loads_both_available_capabilities(self):
        self.ui.load_components()
        self.ui.component_graphics.set(True); self.ui.components_changed()
        saved = components.load(self.ui.state_dir, self.game)['selection']
        self.assertTrue(saved['dlss']); self.assertTrue(saved['effects'])
        self.assertFalse(self.status['components']['dlss'])
        self.ui.component_dlss.set(False); self.ui.load_components()
        self.assertTrue(self.ui.component_dlss.get())
        self.assertTrue(self.ui.component_graphics.get())
        self.ui.game.set(str(self.root / 'other unopened game')); self.ui.load_components()
        self.assertFalse(self.ui.component_dlss.get())
        self.assertFalse(self.ui.component_graphics.get())
        self.assertFalse(self.game.exists())

    def test_missing_dlss_does_not_block_world_effects(self):
        self.ui.load_components(); self.status['dlss_available'] = False
        self.ui.component_graphics.set(True); self.ui.components_changed()
        self.assertFalse(self.ui.component_dlss.get())
        saved = components.load(self.ui.state_dir, self.game)['selection']
        self.assertFalse(saved['dlss']); self.assertTrue(saved['effects'])
        self.assertNotIn('error', self.ui.set_message.call_args.kwargs)

    def test_umbrella_available_for_either_bundle_or_saved_missing_choice(self):
        ui = self.ui
        ui.controls, ui.mode_controls = [], []
        for name in ('game_entry', 'shortcut_button', 'install_button', 'menu', 'launch_button', 'version_label',
                     'app_update_label', 'installer_update_button', 'pack_description', 'pack_update_label',
                     'pack_versions_label', 'download_button', 'download_label', 'install_label'):
            setattr(ui, name, Mock())
        ui.component_controls = {key: Mock() for key in ('textures', 'effects')}
        ui.button_state = Mock(); ui.legacy_uninstall_index = 1
        ui.app_version, ui.update_check, ui.dds_source, ui.release = '1.10.3', {}, {}, {}
        self.status['effects_available'] = False
        with patch.object(app, 'game_executable', return_value=self.game / 'EverQuest2.exe'):
            for available, busy, error, unknown, expected in ((True, None, '', False, 'normal'), (False, None, '', False, 'disabled'),
                    (True, 'install', '', False, 'disabled'), (True, None, 'bad saved selection', False, 'disabled'),
                    (True, None, '', True, 'disabled')):
                self.status['dlss_available'], ui.busy, ui.component_error = available, busy, error
                self.status['unknown'] = unknown
                self.status['components'] = None if unknown else components.normalize()
                app.TextureQuestApp.refresh_controls(ui)
                self.assertEqual(ui.component_controls['effects'].configure.call_args.kwargs['state'], expected)
                self.assertEqual(ui.component_controls['textures'].configure.call_args.kwargs['state'], 'disabled')
            ui.busy, ui.component_error = None, ''
            self.status.update(unknown=False, components=components.normalize())
            self.status['dlss_available'] = False; ui.component_graphics.set(True)
            app.TextureQuestApp.refresh_controls(ui)
            self.assertEqual(ui.component_controls['effects'].configure.call_args.kwargs['state'], 'normal')
            ui.component_graphics.set(False); ui.components_changed()
            self.assertFalse(components.load(ui.state_dir, self.game)['selection']['dlss'])

    def test_apply_changes_preflight_refusal_keeps_working_hook(self):
        self.ui.component_graphics.set(True)
        self.ui.prepared = {'distribution': 'generated-channels-only'}
        self.ui.graphics_manager.preflight_components.side_effect = ValueError('selected dependency changed')
        with self.assertRaisesRegex(ValueError, 'dependency changed'):
            app.TextureQuestApp.manage_graphics(self.ui)
        self.ui.graphics_manager.uninstall.assert_not_called()
        self.ui.graphics_manager.install.assert_not_called()

    def test_loading_each_legacy_selection_keeps_flags_and_record_unchanged(self):
        for effects, dlss in ((False, False), (True, False), (False, True), (True, True)):
            with self.subTest(effects=effects, dlss=dlss):
                game = self.root / f'legacy-{effects}-{dlss}'
                selection = dict(components.normalize(), effects=effects, dlss=dlss)
                saved = components.save(self.ui.state_dir, game, selection, expected_before=None)
                self.ui.game.set(str(game))
                with patch.object(components, 'save') as save:
                    self.ui.load_components()
                    self.assertEqual(self.ui.selected_components(), selection)
                    self.assertEqual(self.ui.component_graphics.get(), effects or dlss)
                    save.assert_not_called()
                self.assertEqual(components.load(self.ui.state_dir, game), saved)

    def test_explicit_apply_expands_legacy_single_bundle_without_checkbox_flip(self):
        self.ui.prepared = {'distribution': 'generated-channels-only'}
        self.ui.graphics_manager.uninstall.return_value = {'kept': []}
        for effects, dlss in ((True, False), (False, True)):
            with self.subTest(effects=effects, dlss=dlss):
                game = self.root / f'apply-{effects}-{dlss}'
                legacy = dict(components.normalize(), effects=effects, dlss=dlss)
                components.save(self.ui.state_dir, game, legacy, expected_before=None)
                self.ui.game.set(str(game)); self.ui.load_components()
                self.status['components'] = legacy
                self.assertTrue(self.ui.component_graphics.get())
                self.assertEqual(self.ui.selected_components(), legacy)
                self.assertTrue(app.TextureQuestApp.manage_graphics(self.ui))
                expected = dict(components.normalize(), effects=True, dlss=True)
                self.assertEqual(self.ui.graphics_manager.install.call_args.kwargs['components'], expected)
                self.assertEqual(components.load(self.ui.state_dir, game)['selection'], expected)
                self.ui.graphics_manager.preflight_components.assert_called_with(str(game), expected)
                self.ui.dds_store.verify.assert_called_with(self.ui.prepared, full=True)

    def test_automatic_upgrade_reconciles_legacy_flags_with_selected_group(self):
        self.ui.update_check = {}; self.ui.release = {}
        self.ui.downloads_in_use = Mock(); self.ui.report_overall = Mock()
        self.ui.pause = types.SimpleNamespace(is_set=lambda: False)
        choices = self.root / 'choices.json'; choices.write_text('{}')
        for effects, dlss in ((True, False), (False, True)):
            with self.subTest(effects=effects, dlss=dlss):
                self.ui.load_components()
                legacy = dict(components.normalize(), effects=effects, dlss=dlss)
                self.ui.save_components(legacy)
                before = components.load(self.ui.state_dir, self.game)
                with patch.object(app, 'resource', return_value=choices), \
                     patch('texturequest_setup.prepare_graphics', return_value=({}, {'texture_version': '1.3.0'})) as prepare:
                    app.TextureQuestApp._download_latest(self.ui, auto_install=True, upgrading=True)
                expected = dict(components.normalize(), effects=True, dlss=True)
                self.assertEqual(prepare.call_args.kwargs['components'], expected)
                after = components.load(self.ui.state_dir, self.game)
                self.assertEqual(after['selection'], expected)
                self.assertNotEqual(after['record_sha256'], before['record_sha256'])

    def test_unchecking_saved_missing_bundles_keeps_required_textures(self):
        self.ui.load_components()
        self.ui.save_components(dict(components.normalize(), effects=True, dlss=True))
        self.status.update(effects_available=False, dlss_available=False)
        self.ui.component_graphics.set(False); self.ui.components_changed()
        self.assertEqual(components.load(self.ui.state_dir, self.game)['selection'], components.normalize())
        self.assertFalse(self.ui.component_graphics.get())

    def test_unknown_status_cannot_erase_preferences_on_toggle_or_apply(self):
        self.ui.load_components()
        self.ui.save_components(dict(components.normalize(), effects=True, dlss=True))
        before = components.load(self.ui.state_dir, self.game)
        self.status = dict(unknown=True, available=False, installed=False, message='Cannot inspect graphics runtime')
        for action in (self.ui.components_changed, lambda: app.TextureQuestApp.manage_graphics(self.ui)):
            with self.subTest(action=action):
                self.ui.component_graphics.set(False)
                with patch.object(components, 'save', wraps=components.save) as save:
                    action()
                    save.assert_not_called()
                self.assertEqual(components.load(self.ui.state_dir, self.game), before)
                self.assertTrue(self.ui.component_graphics.get())
        self.ui.begin.assert_not_called()
        self.ui.graphics_manager.uninstall.assert_not_called()
        self.ui.graphics_manager.install.assert_not_called()

    def test_checkbox_projects_one_known_status_snapshot_before_save(self):
        self.ui.load_components(); self.ui.component_graphics.set(True)
        known = dict(self.status)
        self.ui.graphics_status = Mock(side_effect=[known, dict(unknown=True)])
        self.ui.components_changed()
        self.ui.graphics_status.assert_called_once_with(str(self.game))
        self.assertEqual(components.load(self.ui.state_dir, self.game)['selection'],
                         dict(components.normalize(), effects=True, dlss=True))

    def setup_flow(self, failure=False):
        order = []
        state = self.root / 'setup-data'; state.mkdir()
        prepared = dict(distribution='generated-channels-only')
        store = Mock(); store.prepare.return_value = prepared
        store.verify_for_install.side_effect = lambda *a, **kw: order.append('verified-textures')
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
