"""Two-mode install and legacy-removal flow with fake manager/process leaves and own Tk window."""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import tkinter as tk
import unittest
from unittest.mock import MagicMock, patch

SRC = Path(__file__).resolve().parents[1]/'src'
sys.path.insert(0, str(SRC))
import texturequest_app as ui


class TwoModeUI(unittest.TestCase):
    def setUp(self):
        home = Path(__file__).resolve().parents[1]/'build'
        home.mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=home, prefix='three-modes-')
        self.home = Path(self.tmp.name)
        self.game = self.home/'game'; (self.game/'paks').mkdir(parents=True)
        (self.game/'EverQuest2.exe').write_bytes(b'never executed')
        self.memory, self.graphics = MagicMock(), MagicMock()
        self.mem_state = dict(installed=False,running=False,active_sessions=0,available=True,message='Memory hook ready')
        self.gfx_state = dict(installed=False,running=False,active_sessions=0,available=True,message='Graphics hook ready',dual_load=False)
        self.memory.status.side_effect=lambda game:dict(self.mem_state)
        self.graphics.status.side_effect=lambda game:dict(self.gfx_state)
        self.memory.cache_in_use.side_effect=lambda:bool(self.mem_state['installed'] or self.mem_state['active_sessions'])
        self.graphics.cache_in_use.side_effect=lambda:bool(self.gfx_state['installed'] or self.gfx_state['active_sessions'])
        self.memory.installed_game.return_value=None
        self.root=tk.Tk(); self.root.withdraw()
        self.stack=ExitStack()
        self.stack.enter_context(patch.object(ui,'LoaderManager',return_value=self.memory))
        self.stack.enter_context(patch.object(ui,'GraphicsManager',return_value=self.graphics))
        self.app=ui.TextureQuestApp(self.root, self.home/'data', initial_game=self.game,startup_check=False)
        self.app.prepared={'schema':'texturequest-direct-prepared-v1','id':'new-fixture'}
        self.app.preflight={'missing':[],'archives':[],'paks_dir':str(self.game/'paks')}
        self.app.release_checked=True
        self.begin=self.stack.enter_context(patch.object(self.app,'begin'))
        self.app.refresh_controls(); self.root.update_idletasks()

    def tearDown(self):
        self.stack.close()
        for c in self.root.tk.call('after','info'):self.root.after_cancel(c)
        self.root.destroy(); self.tmp.cleanup()

    def test_only_two_choices_default_graphics_and_no_dual_widget(self):
        self.assertEqual([c.cget('text') for c in self.app.mode_controls],['Replace PAK files','Graphics hook (default)'])
        self.assertEqual(self.app.install_mode.get(),'graphics')
        self.assertEqual(ui.default_install_mode(dict(installed=True)),'graphics')
        self.assertFalse(hasattr(self.app,'dual_load_control'))
        self.assertFalse(hasattr(self.app,'dual_load'))
        self.assertEqual(self.app.shortcut_button.cget('text'),'Shortcut: Numpad *')
        self.graphics.install.assert_not_called();self.memory.install.assert_not_called()

    def test_new_graphics_install_always_uses_on_demand_and_keeps_hotkey(self):
        self.app.install_selected()
        self.assertEqual(self.begin.call_args.args[0],'graphics-install')
        self.begin.call_args.args[2]()
        self.assertIs(self.graphics.install.call_args.kwargs['dual_load'],False)
        self.assertEqual(self.graphics.install.call_args.kwargs['shortcut'],{'vk':106,'modifiers':0})
        self.memory.install.assert_not_called()

    def test_old_dual_setting_never_silently_rewritten(self):
        self.gfx_state.update(installed=True,running=True,dual_load=True)
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget('text'),'Uninstall graphics hook')
        self.graphics.install.assert_not_called();self.graphics.uninstall.assert_not_called()

    def test_graphics_ownership_blocks_pak_install(self):
        self.gfx_state.update(installed=True,running=True)
        self.app.mode_controls[0].invoke()
        self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.app.install_selected();self.begin.assert_not_called()
        self.app.mode_controls[1].invoke()
        self.assertEqual(self.app.install_button.cget('text'),'Uninstall graphics hook')

    def test_legacy_memory_removal_only_then_wait_for_active_game(self):
        self.mem_state.update(installed=True,running=True,active_sessions=1)
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.assertIn('Settings',self.app.install_label.cget('text'))
        self.assertEqual(self.app.menu.entrycget(self.app.legacy_uninstall_index,'state'),'normal')
        def remove(game,progress):self.mem_state.update(installed=False,running=False)
        self.memory.uninstall.side_effect=remove
        self.app.menu.invoke(self.app.legacy_uninstall_index)
        self.assertEqual(self.begin.call_args.args[0],'loader-uninstall')
        result=self.begin.call_args.args[2]();self.assertFalse(result['installed'])
        self.app.refresh_controls()
        self.assertIn('Close EQ2',self.app.install_label.cget('text'))
        self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.mem_state.update(active_sessions=0);self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget('state'),'normal')
        self.memory.install.assert_not_called()

    def test_unknown_or_missing_legacy_install_never_uninstalls(self):
        self.assertFalse(self.app.uninstall_legacy_loader())
        self.memory.status.side_effect=ValueError('invented changed record')
        self.app.refresh_controls()
        self.assertEqual(self.app.menu.entrycget(self.app.legacy_uninstall_index,'state'),'disabled')
        self.assertFalse(self.app.uninstall_legacy_loader())
        self.memory.uninstall.assert_not_called();self.memory.install.assert_not_called()

    def test_removed_mode_cannot_fall_through_to_pak_or_memory_install(self):
        self.app.install_mode.set('background')
        self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.app.install_selected();self.begin.assert_not_called()
        self.memory.install.assert_not_called();self.graphics.install.assert_not_called()

    def test_missing_runtime_never_falls_through_to_pak(self):
        self.gfx_state.update(available=False,message='Missing runtime')
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.assertFalse(self.app.manage_graphics());self.begin.assert_not_called()

    def test_controls_fit_without_scroll_and_shortcut_only_in_graphics(self):
        self.root.attributes('-alpha',0.0);self.root.deiconify();self.root.update()
        bottom=self.root.winfo_rooty()+self.root.winfo_height()
        for widget in [*self.app.mode_controls,self.app.shortcut_button,self.app.install_button,self.app.launch_button,self.app.progress]:
            self.assertLessEqual(widget.winfo_rooty()+widget.winfo_height(),bottom)
        self.app.mode_controls[0].invoke();self.assertFalse(self.app.shortcut_button.winfo_manager())
        self.app.mode_controls[1].invoke();self.assertTrue(self.app.shortcut_button.winfo_manager())

    def test_shortcut_changes_without_installing_or_restarting(self):
        self.app.apply_graphics_shortcut({'vk':119,'modifiers':2})
        self.assertEqual(self.app.shortcut_button.cget('text'),'Shortcut: Ctrl+F8')
        self.begin.assert_not_called()
        self.gfx_state.update(installed=True,running=True)
        self.app.apply_graphics_shortcut({'vk':106,'modifiers':0})
        self.assertEqual(self.begin.call_args.args[0],'graphics-shortcut')
        self.begin.call_args.args[2]();self.graphics.set_shortcut.assert_called_once()
        self.graphics.install.assert_not_called();self.graphics.uninstall.assert_not_called()

if __name__=='__main__':unittest.main()
