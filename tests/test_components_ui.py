"""Real compact Tk UI with invented games and generated-channel fixtures."""
from pathlib import Path
import tkinter as tk
from unittest.mock import patch

from test_loose_dds_ui import GraphicsOnlyUI
from test_effects_install import enable_fixture
import texturequest_app as app
import texturequest_components as components
import texturequest_quality as quality


class ComponentUI(GraphicsOnlyUI):
    # Inherited baseline tests keep separate original scope; only these new
    # names are selected by the focused runner for this class.
    def setUp(self):
        super().setUp()
        self.stack.enter_context(patch('texturequest_game_settings.ensure_closed'))
        enable_fixture(self.runtime)
        self.ui.refresh_controls()

    def test_required_textures_unavailable_dlss_and_optional_effects_default(self):
        self.assertTrue(self.ui.component_textures.get())
        self.assertFalse(self.ui.component_dlss.get())
        self.assertFalse(self.ui.component_effects.get())
        self.assertFalse(self.ui.component_graphics.get())
        self.assertEqual(self.ui.component_controls['textures'].cget('state'), 'disabled')
        self.assertEqual(set(self.ui.component_controls), {'textures', 'effects'})
        self.assertEqual(self.ui.component_controls['effects'].cget('text'), 'Graphical Effects / DLSS')
        self.assertEqual(self.ui.graphics_settings_note.cget('text'), 'Press Home to show or hide settings. Choose TextureQuest.')
        self.assertEqual(self.ui.component_controls['effects'].cget('state'), 'normal')

    def test_saved_choice_is_per_game_and_not_installed_capability(self):
        self.ui.component_graphics.set(True); self.ui.components_changed()
        self.assertTrue(components.load(self.state, self.game)['selection']['effects'])
        self.assertFalse(self.manager.status(self.game)['installed'])
        other = self.root/'Other game'
        self.ui.game.set(str(other)); self.assertFalse(self.ui.component_effects.get())
        self.ui.game.set(str(self.game)); self.assertTrue(self.ui.component_effects.get())

    def test_change_installed_components_reuses_textures_and_restores_user_settings(self):
        from test_channels import fixture
        game, source, index, examples, choices, opener, requests = fixture(self.root/'current-channel-input')
        self.game = game; self.ui.game.set(str(game))
        prepared = self.ui.dds_store.prepare(source, game, choices, opener=opener)
        self.ui.prepared = prepared; self.ui.release_checked = True
        self.manager.install(self.game, prepared)
        self.ui.upgrade.complete(self.game, prepared)
        self.ui.component_graphics.set(True); self.ui.components_changed()
        self.assertEqual(self.ui.install_button.cget('text'), 'Apply changes')
        with patch.object(self.ui, 'begin', side_effect=lambda kind,message,action: action()):
            self.assertTrue(self.ui.manage_graphics())
        self.assertTrue(self.manager.status(self.game)['components']['effects'])
        self.ui.component_graphics.set(False); self.ui.components_changed()
        with patch.object(self.ui, 'begin', side_effect=lambda kind,message,action: action()):
            self.assertTrue(self.ui.manage_graphics())
        self.assertFalse(self.manager.status(self.game)['components']['effects'])
        self.assertIn(b'NoReloadOnInit=1', (self.game/'ReShade.ini').read_bytes())

    def test_components_and_launch_fit_default_window(self):
        self.tk.geometry('960x640'); self.tk.attributes('-alpha',0); self.tk.deiconify(); self.tk.update()
        for widget in [*self.ui.component_controls.values(), self.ui.quality_dropdown, self.ui.quality_note,
                       self.ui.graphics_settings_note, self.ui.install_button, self.ui.launch_button]:
            self.assertGreaterEqual(widget.winfo_rooty(), self.tk.winfo_rooty())
            self.assertLessEqual(widget.winfo_rooty()+widget.winfo_height(), self.tk.winfo_rooty()+self.tk.winfo_height())

    def test_checked_legacy_single_bundle_shows_apply_without_saving_on_refresh(self):
        for effects, dlss in ((True, False), (False, True)):
            with self.subTest(effects=effects, dlss=dlss):
                game = self.root / f'legacy-apply-{effects}-{dlss}'
                legacy = dict(components.normalize(), effects=effects, dlss=dlss)
                before = components.save(self.state, game, legacy, expected_before=None)
                self.ui.game.set(str(game))
                status = dict(self.manager.status(game), installed=True, components=legacy,
                              effects_available=True, dlss_available=True)
                with patch.object(self.ui, 'graphics_status', return_value=status), \
                     patch.object(self.ui, 'upgrade_pending', return_value=False):
                    self.ui.refresh_controls()
                self.assertTrue(self.ui.component_graphics.get())
                self.assertEqual(self.ui.install_button.cget('text'), 'Apply changes')
                self.assertEqual(self.ui.selected_components(), legacy)
                self.assertEqual(components.load(self.state, game), before)

    def test_stale_installed_record_refuses_before_removal(self):
        prepared = self.prepared()
        self.manager.install(self.game, prepared)
        self.ui.prepared = prepared; self.ui.release_checked = True
        self.ui.component_graphics.set(True)
        with patch.object(self.ui, 'upgrade_pending', return_value=False):
            def changed(kind, message, action):
                record = self.manager._record(self.game)
                record['unrelated_later_action'] = 'another installer action'
                self.manager._save(self.game, record)
                return action()
            with patch.object(self.ui, 'begin', side_effect=changed), patch.object(self.manager, 'uninstall') as remove:
                with self.assertRaisesRegex(RuntimeError, 'installation state changed'):
                    self.ui.manage_graphics()
                remove.assert_not_called()

    def test_preference_lock_contention_restores_saved_choice(self):
        self.ui.component_graphics.set(True)
        with patch.object(components, 'save', side_effect=RuntimeError('another action holds the lock')):
            self.ui.components_changed()
        self.assertFalse(self.ui.component_effects.get())
        self.assertIn('another action holds the lock', self.ui.message.get('1.0','end-1c'))

    def test_component_change_cannot_downgrade_newer_hook_with_prepared_cache(self):
        prepared = self.prepared()
        self.manager.install(self.game, prepared)
        record = self.manager._record(self.game)
        record['app_version'] = '9999.0.0'
        self.manager._save(self.game, record)
        self.ui.prepared = prepared; self.ui.release_checked = True
        self.ui.component_graphics.set(True)
        with patch.object(self.manager, 'uninstall') as remove, \
             patch.object(self.manager, 'install') as install, \
             patch.object(self.ui, 'begin') as begin:
            self.assertFalse(self.ui.manage_graphics())
        remove.assert_not_called(); install.assert_not_called(); begin.assert_not_called()
        self.assertIn('newer TextureQuest app', self.ui.message.get('1.0','end-1c'))

    def _install_quality_fixture(self):
        import test_world_renderer_install as world
        world.WorldRendererInstall.renderer_runtime(self)
        selected = dict(components.normalize(), effects=True)
        with patch.object(self.manager, '_activate_prepared', return_value=self.state / 'invented-map.bin'):
            self.manager.install(self.game, {'release_sha256': 'a' * 64}, components=selected)
        self.ui.save_components(selected)
        self.ui.refresh_controls()

    def test_quality_only_apply_uses_dropdown_without_texture_or_hook_work(self):
        self._install_quality_fixture()
        with patch.object(self.ui, 'upgrade_pending', return_value=False):
            self.ui.quality_choice.set('High'); self.ui.quality_changed()
            self.assertEqual(self.ui.install_button.cget('text'), 'Apply changes')
        self.ui.prepared = None; self.ui.release_checked = False
        with patch.object(self.ui, 'upgrade_pending', return_value=False), \
             patch.object(self.ui, '_download_latest') as download, \
             patch.object(self.manager, 'install') as install, \
             patch.object(self.manager, 'uninstall') as remove, \
             patch.object(self.ui, 'begin', side_effect=lambda kind, message, action: action()):
            self.assertTrue(self.ui.manage_graphics())
        download.assert_not_called(); install.assert_not_called(); remove.assert_not_called()
        self.assertEqual(quality.detect((self.game / 'ReShade.ini').read_bytes()), 'High')
        self.assertIsNone(quality.load_pending(self.state, self.game)['choice'])
        self.ui.refresh_controls()
        self.assertEqual(self.ui.quality_choice.get(), 'High')

    def test_app_refresh_does_not_consume_pending_quality(self):
        self._install_quality_fixture()
        self.ui.quality_choice.set('Max'); self.ui.quality_changed()
        before = (self.game / 'ReShade.ini').read_bytes()
        request = quality.load_pending(self.state, self.game)
        with patch('texturequest_setup.prepare_graphics', return_value=({}, {})), \
             patch.object(self.ui, 'begin', side_effect=lambda kind, message, action: action()):
            self.ui._download_latest(auto_install=True, upgrading=True)
        self.assertEqual((self.game / 'ReShade.ini').read_bytes(), before)
        self.assertEqual(quality.load_pending(self.state, self.game), request)

