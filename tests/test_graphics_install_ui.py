"""New three-mode UI flow with fake manager/process leaves and own Tk window."""
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


class ThreeModeUI(unittest.TestCase):
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

    def test_three_exclusive_choices_and_dual_load_no_install_side_effect(self):
        self.assertEqual([c.cget('text') for c in self.app.mode_controls],['Replace PAK files','Memory hook','Graphics hook (default)'])
        self.assertEqual(self.app.install_mode.get(),'graphics')
        self.assertEqual(self.app.shortcut_button.cget('text'),'Shortcut: Numpad *')
        self.app.mode_controls[2].invoke()
        self.assertEqual(self.app.install_mode.get(),'graphics')
        self.assertFalse(self.app.dual_load.get())
        self.app.dual_load_control.invoke(); self.assertTrue(self.app.dual_load.get())
        self.assertEqual(self.app.install_button.cget('text'),'Install graphics hook')
        self.graphics.install.assert_not_called(); self.memory.install.assert_not_called(); self.begin.assert_not_called()
        self.app.mode_controls[0].invoke(); self.assertFalse(self.app.graphics_selected())
        self.assertEqual(self.app.dual_load_control.winfo_manager(),'')

    def test_explicit_graphics_install_passes_checkbox_and_uninstall_preserves_other_modes(self):
        self.app.mode_controls[2].invoke(); self.app.dual_load.set(True)
        def install(game,prepared,dual_load,progress,shortcut):
            self.assertEqual(game,str(self.game)); self.assertTrue(dual_load)
            self.assertEqual(shortcut,{'vk':106,'modifiers':0})
            self.gfx_state.update(installed=True,running=True,dual_load=True)
            return dict(self.gfx_state)
        self.graphics.install.side_effect=install
        self.app.install_selected(); self.assertEqual(self.begin.call_args.args[0],'graphics-install')
        self.begin.call_args.args[2]()
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget('text'),'Uninstall graphics hook')
        self.assertEqual(self.app.dual_load_control.cget('state'),'disabled')
        self.app.mode_controls[0].invoke(); self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.app.mode_controls[1].invoke(); self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.memory.install.assert_not_called()

    def test_memory_install_is_refused_while_graphics_is_owned(self):
        self.gfx_state.update(installed=True,running=True)
        self.app.mode_controls[1].invoke()
        self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.app.manage_loader()
        if self.begin.called:
            with self.assertRaises(RuntimeError):self.begin.call_args.args[2]()
        self.memory.install.assert_not_called()

    def test_missing_runtime_cannot_install_or_fall_through_to_pak(self):
        self.gfx_state.update(available=False,message='Full-pack graphics runtime unavailable')
        self.app.mode_controls[2].invoke()
        self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.assertFalse(self.app.manage_graphics())
        self.begin.assert_not_called(); self.graphics.install.assert_not_called()

    def test_compact_selected_description_and_installed_mode_default(self):
        self.assertEqual(ui.default_install_mode(self.mem_state,dict(self.gfx_state,installed=True)),'graphics')
        for i in range(3):
            self.app.mode_controls[i].invoke(); self.root.update_idletasks()
            self.assertEqual(sum(bool(d.winfo_manager()) for d in self.app.mode_descriptions),1)
        # No scrolling body introduced; three choices occupy the existing card.
        self.assertLessEqual(self.app.root.winfo_reqheight(),750)

    def test_installed_graphics_controls_fit_without_scrolling(self):
        self.gfx_state.update(installed=True,running=True,message='Graphics hook installed. Ctrl + Alt + F10 switches Original / Upscaled.')
        self.app.mode_controls[2].invoke()
        self.root.attributes('-alpha',0.0)
        self.root.deiconify();self.root.update()
        bottom=self.root.winfo_rooty()+self.root.winfo_height()
        for widget in [*self.app.mode_controls,self.app.dual_load_control,self.app.shortcut_button,self.app.install_button,self.app.launch_button,self.app.progress]:
            self.assertLessEqual(widget.winfo_rooty()+widget.winfo_height(),bottom)
        self.assertEqual(self.app.install_button.cget('text'),'Uninstall graphics hook')

    def test_shortcut_customization_is_saved_without_reinstall_or_texture_actions(self):
        self.app.apply_graphics_shortcut({'vk':119,'modifiers':2})
        self.assertEqual(self.app.shortcut_button.cget('text'),'Shortcut: Ctrl+F8')
        self.begin.assert_not_called()
        self.gfx_state.update(installed=True,running=True)
        self.app.apply_graphics_shortcut({'vk':106,'modifiers':0})
        self.assertEqual(self.begin.call_args.args[0],'graphics-shortcut')
        self.begin.call_args.args[2]()
        self.graphics.set_shortcut.assert_called_once()
        self.graphics.install.assert_not_called();self.graphics.uninstall.assert_not_called()


if __name__=='__main__': unittest.main()
