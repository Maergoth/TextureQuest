"""Real compact Tk UI with invented games and generated-channel fixtures."""
from pathlib import Path
import tkinter as tk
from unittest.mock import patch

from test_loose_dds_ui import GraphicsOnlyUI
from test_effects_install import enable_fixture
import texturequest_app as app
import texturequest_components as components


class ComponentUI(GraphicsOnlyUI):
    # Inherited baseline tests keep separate original scope; only these new
    # names are selected by the focused runner for this class.
    def setUp(self):
        super().setUp()
        enable_fixture(self.runtime)
        self.ui.refresh_controls()

    def test_required_textures_unavailable_dlss_and_optional_effects_default(self):
        self.assertTrue(self.ui.component_textures.get())
        self.assertFalse(self.ui.component_dlss.get())
        self.assertFalse(self.ui.component_effects.get())
        self.assertEqual(self.ui.component_controls['textures'].cget('state'), 'disabled')
        self.assertEqual(self.ui.component_controls['dlss'].cget('state'), 'disabled')
        self.assertEqual(self.ui.component_controls['effects'].cget('state'), 'normal')

    def test_saved_choice_is_per_game_and_not_installed_capability(self):
        self.ui.component_effects.set(True); self.ui.components_changed()
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
        self.ui.component_effects.set(True); self.ui.components_changed()
        self.assertEqual(self.ui.install_button.cget('text'), 'Apply changes')
        with patch.object(self.ui, 'begin', side_effect=lambda kind,message,action: action()):
            self.assertTrue(self.ui.manage_graphics())
        self.assertTrue(self.manager.status(self.game)['components']['effects'])
        self.ui.component_effects.set(False); self.ui.components_changed()
        with patch.object(self.ui, 'begin', side_effect=lambda kind,message,action: action()):
            self.assertTrue(self.ui.manage_graphics())
        self.assertFalse(self.manager.status(self.game)['components']['effects'])
        self.assertIn(b'NoReloadOnInit=1', (self.game/'ReShade.ini').read_bytes())

    def test_components_and_launch_fit_default_window(self):
        self.tk.geometry('960x640'); self.tk.attributes('-alpha',0); self.tk.deiconify(); self.tk.update()
        for widget in [*self.ui.component_controls.values(), self.ui.install_button, self.ui.launch_button]:
            self.assertGreaterEqual(widget.winfo_rooty(), self.tk.winfo_rooty())
            self.assertLessEqual(widget.winfo_rooty()+widget.winfo_height(), self.tk.winfo_rooty()+self.tk.winfo_height())

    def test_stale_installed_record_refuses_before_removal(self):
        prepared = self.prepared()
        self.manager.install(self.game, prepared)
        self.ui.prepared = prepared; self.ui.release_checked = True
        self.ui.component_effects.set(True)
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
        self.ui.component_effects.set(True)
        with patch.object(components, 'save', side_effect=RuntimeError('another action holds the lock')):
            self.ui.components_changed()
        self.assertFalse(self.ui.component_effects.get())
        self.assertIn('another action holds the lock', self.ui.message.get('1.0','end-1c'))

