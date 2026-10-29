"""New mode-help UI cases only; fresh D fixture and mocked installer leaves."""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import tkinter as tk
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_app as module


class InstallModeInfoUI(unittest.TestCase):
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="mode-info-new-", dir=base)
        self.folder = Path(self.temp.name)
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.tk.call("tk", "scaling", 96 / 72)
        self.root.report_callback_exception = self.callback_error
        self.callback_errors = []
        self.stack = ExitStack()
        self.manifest = dict(schema="texturequest-release-v3", version="1.0.0", texture_version="1.0.0",
                             texture_count=1, archives=[dict(name="fixture.vpk", bytes=1, sha256="0" * 64)])
        self.manager = MagicMock()
        self.loader = dict(installed=False, running=False, active_sessions=0, available=True, message="")
        self.manager.status.side_effect = lambda _game: dict(self.loader)
        self.manager.cache_in_use.side_effect = lambda: self.loader["installed"]
        self.manager.installed_game.return_value = None
        self.swapper = MagicMock()
        self.swapper.saved_ready.return_value = None
        self.stack.enter_context(patch.object(module, "DirectSwapper", return_value=self.swapper))
        self.stack.enter_context(patch.object(module, "LoaderManager", return_value=self.manager))
        self.stack.enter_context(patch.object(module, "load_manifest", return_value=self.manifest))
        self.stack.enter_context(patch.object(module, "saved_manifest", return_value=self.manifest))
        self.stack.enter_context(patch.object(module, "game_executable", side_effect=ValueError("invented fixture")))

    def callback_error(self, *error):
        self.callback_errors.append(error)

    def tearDown(self):
        self.stack.close()
        if self.root.winfo_exists():
            for callback in self.root.tk.call("after", "info"):
                self.root.after_cancel(callback)
            self.root.destroy()
        self.temp.cleanup()
        self.assertEqual(self.callback_errors, [])

    def app(self, installed=False):
        self.loader.update(installed=installed, running=installed)
        with patch.object(self.root, "winfo_screenwidth", return_value=1366), \
             patch.object(self.root, "winfo_screenheight", return_value=768), \
             patch.object(self.root, "winfo_fpixels", return_value=96):
            app = module.TextureQuestApp(self.root, self.folder / "data",
                                        initial_game=self.folder / "invented game", startup_check=False)
        for callback in self.root.tk.call("after", "info"):
            self.root.after_cancel(callback)
        self.root.deiconify()
        self.root.update()
        return app

    def tooltip(self, text=None):
        self.root.geometry("600x400+20+20")
        self.icon = tk.Label(self.root, text="ⓘ", takefocus=True)
        self.icon.place(x=560, y=365)
        self.root.deiconify()
        self.root.update()
        return module.HoverInfo(self.icon, text or module.INSTALL_MODE_INFO[1][3], delay_ms=0)

    def test_mode_copy_is_adjacent_compact_and_does_not_change_selection(self):
        app = self.app()
        self.assertEqual(app.install_mode.get(), "pak")
        self.assertEqual([w.cget("text") for w in app.mode_controls],
                         ["Replace PAK files", "Background texture loader"])
        self.assertEqual([w.cget("text") for w in app.mode_descriptions],
                         ["Recommended: no runtime injection.", "Private cache; game PAKs untouched."])
        self.assertEqual(len(app.mode_info_icons), 2)
        for radio, icon, description in zip(app.mode_controls, app.mode_info_icons, app.mode_descriptions):
            self.assertEqual(icon.master, radio.master)
            self.assertGreaterEqual(icon.winfo_x(), radio.winfo_x() + radio.winfo_width())
            self.assertGreaterEqual(description.winfo_y(), radio.winfo_y() + radio.winfo_height())
            self.assertLessEqual(icon.winfo_rootx() + icon.winfo_width(), self.root.winfo_rootx() + 960)
        for widget in (app.install_button, app.launch_button, app.mode_descriptions[-1], app.message):
            self.assertLessEqual(widget.winfo_rooty() + widget.winfo_height(), self.root.winfo_rooty() + 640)
        app.mode_controls[1].invoke()
        self.assertEqual(app.install_mode.get(), "background")
        app.mode_controls[0].invoke()
        self.assertEqual(app.install_mode.get(), "pak")
        self.manager.install.assert_not_called()
        self.manager.uninstall.assert_not_called()
        self.swapper.install.assert_not_called()

    def test_recommendation_preserves_owned_background_default_and_writer_guard(self):
        app = self.app(installed=True)
        self.assertEqual(app.install_mode.get(), "background")
        self.assertEqual(app.install_button.cget("text"), "Uninstall background loader")
        app.mode_controls[0].invoke()
        self.assertEqual(app.install_button.cget("state"), "disabled")
        self.assertIn("Uninstall the background loader", app.install_label.cget("text"))
        self.manager.install.assert_not_called()
        self.manager.uninstall.assert_not_called()
        self.swapper.install.assert_not_called()

    def test_hover_leave_click_and_focus_own_the_popup_without_taking_focus(self):
        hint = self.tooltip()
        self.icon.focus_force()
        self.root.update()
        before = self.root.focus_get()
        self.icon.event_generate("<Enter>")
        self.root.update()
        self.assertIsNotNone(hint.window)
        self.assertEqual(self.root.focus_get(), before)
        self.assertIsNone(self.root.grab_current())
        self.icon.event_generate("<Leave>")
        self.root.update()
        self.assertIsNone(hint.window)
        self.icon.event_generate("<Enter>")
        self.root.update()
        hint.window.event_generate("<ButtonPress-1>")
        self.root.update()
        self.assertIsNone(hint.window)
        self.icon.event_generate("<FocusIn>")
        self.root.update()
        self.assertIsNotNone(hint.window)
        self.root.event_generate("<ButtonPress-1>")
        self.root.update()
        self.assertIsNone(hint.window)
        self.icon.event_generate("<FocusIn>")
        self.root.update()
        self.icon.event_generate("<FocusOut>")
        self.root.update()
        self.assertIsNone(hint.window)

    def test_edge_popup_fits_screen_and_owner_destroy_cancels_pending_or_open_hint(self):
        hint = self.tooltip()
        width, height = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
        self.root.geometry(f"600x400+{max(0, width - 615)}+{max(0, height - 445)}")
        self.root.update()
        hint.show()
        self.root.update()
        window = hint.window
        self.assertGreaterEqual(window.winfo_rootx(), 0)
        self.assertGreaterEqual(window.winfo_rooty(), 0)
        self.assertLessEqual(window.winfo_rootx() + window.winfo_width(), width)
        self.assertLessEqual(window.winfo_rooty() + window.winfo_height(), height)
        self.icon.destroy()
        self.root.update()
        self.assertIsNone(hint.window)
        other = tk.Label(self.root, text="ⓘ")
        other.pack()
        pending = module.HoverInfo(other, "pending", delay_ms=10000)
        pending.schedule()
        callback = pending.pending
        other.destroy()
        self.assertIsNone(pending.pending)
        self.assertNotIn(callback, self.root.tk.call("after", "info"))

    def test_tooltip_copy_explains_both_patch_and_runtime_tradeoffs(self):
        replace, background = module.INSTALL_MODE_INFO
        for phrase in ("EQ2 game folder", "use directly", "No runtime injection", "closed to install",
                       "official EQ2 patcher restores stock", "reinstall textures after patching"):
            self.assertIn(phrase, replace[3])
        for phrase in ("game files unchanged", "per-user background helper", "future EQ2 launches", "private cache",
                       "Loads a DLL", "risk is unquantified", "archive or index", "Uninstall, then Install",
                       "No instant A/B", "until it closes"):
            self.assertIn(phrase, background[3])
        self.assertIn("not a promise of ban safety or official approval", replace[3])

    def test_owner_destroy_clears_open_hint_and_leave_cancels_delayed_hint(self):
        owner = tk.Toplevel(self.root)
        owner.geometry("600x400+30+30")
        icon = tk.Label(owner, text="ⓘ", takefocus=True)
        icon.pack()
        self.root.update()
        hint = module.HoverInfo(icon, "Delayed owner-bound help", delay_ms=10000)
        icon.event_generate("<Enter>")
        self.root.update()
        self.assertIsNotNone(hint.pending)
        self.assertIsNone(hint.window)
        callback = hint.pending
        icon.event_generate("<Leave>")
        self.root.update()
        self.assertIsNone(hint.pending)
        self.assertNotIn(callback, self.root.tk.call("after", "info"))
        hint.show()
        self.root.update()
        popup = hint.window
        self.assertIsNotNone(popup)
        owner.destroy()
        self.root.update()
        self.assertIsNone(hint.window)
        self.assertIsNone(hint.pending)
        self.assertFalse(popup.winfo_exists())


if __name__ == "__main__":
    unittest.main()
