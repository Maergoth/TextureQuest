"""Launcher and success-dialog UI with disposable D fixtures; never starts a game."""
from pathlib import Path
import sys
import tempfile
import tkinter as tk
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_app as app_module


class LauncherUI(unittest.TestCase):
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="launcher-ui-", dir=base)
        self.folder = Path(self.temp.name)
        self.game = self.folder / "selected game folder"
        self.game.mkdir()
        (self.game / "paks").mkdir()
        # File existence is a UI fixture only. Every launch call is mocked.
        (self.game / "EverQuest2.exe").write_bytes(b"not an executable")
        (self.game / "LaunchPad.exe").write_bytes(b"must not be selected")
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.tk.call("tk", "scaling", 96 / 72)
        with patch.object(self.root, "winfo_screenwidth", return_value=1366), \
             patch.object(self.root, "winfo_screenheight", return_value=768), \
             patch.object(self.root, "winfo_fpixels", return_value=96):
            self.app = app_module.TextureQuestApp(self.root, self.folder / "data", startup_check=False)

    def tearDown(self):
        for callback in self.root.tk.call("after", "info"):
            self.root.after_cancel(callback)
        self.root.destroy()
        self.temp.cleanup()

    def select_game(self):
        self.app.game.set(str(self.game))
        self.app.refresh_controls()
        self.root.update_idletasks()

    def test_launch_requires_selected_game_executable_and_is_disabled_while_busy(self):
        self.assertEqual(self.app.launch_button.cget("state"), "disabled")
        self.app.game.set(str(self.folder))
        self.assertEqual(self.app.launch_button.cget("state"), "disabled")
        self.select_game()
        self.assertEqual(self.app.launch_button.cget("state"), "normal")
        self.app.busy = "install"
        self.app.refresh_controls()
        self.assertEqual(self.app.launch_button.cget("state"), "disabled")
        with patch.object(app_module, "start_game") as launch:
            self.app.launch_game()
            launch.assert_not_called()

    def test_main_launch_uses_selected_folder_with_spaces(self):
        self.select_game()
        with patch.object(app_module, "start_game") as launch:
            self.app.launch_button.invoke()
        launch.assert_called_once()
        self.assertEqual(Path(launch.call_args.args[0]), self.game)

    def test_launch_error_is_shown_and_app_remains_available(self):
        self.select_game()
        with patch.object(app_module, "start_game", side_effect=OSError("fixture launch failed")) as launch, \
             patch.object(app_module.messagebox, "showerror") as notice:
            self.app.launch_game()
        launch.assert_called_once()
        notice.assert_called_once_with("Couldn't launch EverQuest II", "fixture launch failed", parent=self.root)
        self.assertIn("fixture launch failed", self.app.message.get("1.0", "end-1c"))
        self.assertFalse(self.app.closing)
        self.assertIsNone(self.app.busy)

    def test_success_dialog_is_nonblocking_and_waits_for_explicit_launch(self):
        self.select_game()
        with patch.object(app_module, "start_game") as launch:
            self.app.events.put(("done", "install", {"status": "installed", "archives": []}))
            self.app.poll()
            launch.assert_not_called()
            dialog = self.app.install_success_dialog
            self.assertIsNotNone(dialog)
            self.assertTrue(dialog.winfo_exists())
            self.assertFalse(dialog.winfo_ismapped())  # Parent is withdrawn.
            self.assertEqual(self.app.success_launch_button.cget("text"), "Launch Game")
            self.assertEqual(self.app.success_close_button.cget("text"), "Close")
            self.app.success_launch_button.invoke()
            launch.assert_called_once()
            self.assertEqual(Path(launch.call_args.args[0]), self.game)

    def test_success_dialog_close_clears_reference_and_preserves_exact_notice(self):
        self.select_game()
        dialog = self.app.show_install_success()
        self.assertIs(dialog, self.app.install_success_dialog)
        pending = [dialog]
        texts = []
        while pending:
            widget = pending.pop()
            pending.extend(widget.winfo_children())
            if "text" in widget.keys():
                texts.append(widget.cget("text"))
        self.assertIn(app_module.INSTALL_SUCCESS_MESSAGE, texts)
        self.assertEqual(app_module.INSTALL_SUCCESS_MESSAGE,
                         "Textures Installed. Launch via the EQ2 EXE, not the launcher!")
        with patch.object(app_module, "start_game") as launch:
            self.app.success_close_button.invoke()
            launch.assert_not_called()
        self.assertIsNone(self.app.install_success_dialog)
        self.assertFalse(dialog.winfo_exists())

    def test_success_dialog_maps_with_visible_parent_and_fits_supported_client(self):
        self.select_game()
        self.root.attributes("-alpha", 0)
        self.root.geometry("960x640")
        self.root.deiconify()
        self.root.update()
        dialog = self.app.show_install_success()
        self.root.update()
        self.assertTrue(dialog.winfo_ismapped())
        self.assertLessEqual(dialog.winfo_width(), self.root.winfo_width())
        self.assertLessEqual(dialog.winfo_height(), self.root.winfo_height())
        for widget in (self.app.success_launch_button, self.app.success_close_button):
            self.assertTrue(widget.winfo_ismapped())
            left = widget.winfo_rootx() - dialog.winfo_rootx()
            top = widget.winfo_rooty() - dialog.winfo_rooty()
            self.assertGreaterEqual(left, 0)
            self.assertGreaterEqual(top, 0)
            self.assertLessEqual(left + widget.winfo_width(), dialog.winfo_width())
            self.assertLessEqual(top + widget.winfo_height(), dialog.winfo_height())

    def test_short_status_needs_no_scrollbar_but_long_error_remains_readable(self):
        self.root.attributes("-alpha", 0)
        self.root.deiconify()
        self.app.set_message("Ready to play.")
        self.root.update()
        self.assertFalse(self.app.status_scrollbar.winfo_ismapped())
        error = "\n".join(f"Archive {i}: verification failed." for i in range(30))
        self.app.set_message(error, error=True)
        self.root.update()
        self.assertTrue(self.app.status_scrollbar.winfo_ismapped())
        self.assertEqual(self.app.message.get("1.0", "end-1c"), error)
        self.app.message.event_generate("<MouseWheel>", delta=-120)
        self.root.update()
        self.assertGreater(self.app.message.yview()[0], 0)


if __name__ == "__main__":
    unittest.main()
