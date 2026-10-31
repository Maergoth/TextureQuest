"""Graphics-only installer wording/actions/layout using disposable Tk fixtures."""
import sys
from pathlib import Path
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_graphics_install_ui import TwoModeUI,ui

class ProxyUI(unittest.TestCase):
    setUp=TwoModeUI.setUp
    tearDown=TwoModeUI.tearDown

    def test_default_choices_and_help_describe_normal_proxy(self):
        self.assertEqual(self.app.install_mode.get(),'graphics')
        self.assertEqual([c.cget('text') for c in self.app.mode_controls],['Replace PAK files','Graphics hook (default)'])
        self.assertFalse(hasattr(self.app,'dual_load_control'))
        help=ui.INSTALL_MODE_INFO[1][3]
        for text in ['TextureQuest.addon64','ReShade add-on build','TextureQuest.ini','No background watcher','after reboot','Shortcut','on demand','ambiguous','patches','Close EQ2','Uninstall']:
            self.assertIn(text,help)
        self.assertEqual(self.app.shortcut_button.cget('text'),'Shortcut: Numpad *')
        self.assertNotIn('sign-in',help)

    def test_installed_no_watcher_is_green_uninstall_and_blocks_replace(self):
        self.gfx_state.update(installed=True,running=False,message='Graphics-only hook installed. No background process.')
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget('text'),'Uninstall graphics hook')
        self.assertEqual(self.app.install_button.cget('bg'),ui.GREEN)
        self.assertIn('No background process',self.app.install_label.cget('text'))
        self.app.mode_controls[0].invoke()
        self.assertEqual(self.app.install_button.cget('state'),'disabled')
        self.app.install_selected();self.begin.assert_not_called()

    def test_new_install_shortcut_and_uninstall_dispatch(self):
        self.app.install_selected();self.begin.call_args.args[2]()
        self.assertFalse(self.graphics.install.call_args.kwargs['dual_load'])
        self.assertEqual(self.graphics.install.call_args.kwargs['shortcut'],dict(vk=106,modifiers=0))
        self.gfx_state.update(installed=True,running=False);self.app.refresh_controls()
        self.app.install_selected();self.begin.call_args.args[2]()
        self.graphics.uninstall.assert_called_once()
        self.memory.install.assert_not_called()

    def test_whole_dashboard_and_full_help_fit_without_scrolling(self):
        self.root.attributes('-alpha',0);self.root.deiconify();self.root.update()
        bottom=self.root.winfo_rooty()+self.root.winfo_height()
        for widget in [*self.app.mode_controls,self.app.shortcut_button,self.app.install_button,self.app.launch_button,self.app.progress]:
            self.assertLessEqual(widget.winfo_rooty()+widget.winfo_height(),bottom)
        hint=ui.HoverInfo(self.app.mode_info_icons[1],ui.INSTALL_MODE_INFO[1][3])
        hint.show();self.root.update();tip=hint.window
        self.assertLessEqual(tip.winfo_rooty()+tip.winfo_height(),bottom)
        self.assertLessEqual(tip.winfo_reqheight(),tip.winfo_height()+2)
        hint.hide()

    def test_shortcut_changes_installed_without_restart(self):
        self.gfx_state.update(installed=True,running=False);self.app.refresh_controls()
        self.app.apply_graphics_shortcut(dict(vk=119,modifiers=2))
        self.assertEqual(self.begin.call_args.args[0],'graphics-shortcut')
        self.begin.call_args.args[2]()
        self.graphics.set_shortcut.assert_called_once()
        self.graphics.install.assert_not_called();self.graphics.uninstall.assert_not_called()


if __name__=='__main__':
    import faulthandler
    faulthandler.dump_traceback_later(12,repeat=False)
    unittest.main(verbosity=2,defaultTest='ProxyUI')
