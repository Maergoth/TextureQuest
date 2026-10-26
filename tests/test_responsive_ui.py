"""Real Tk geometry with disposable D-only state; no network or installation."""
from pathlib import Path
import sys
import tempfile
import threading
import tkinter as tk
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from texturequest_app import TextureQuestApp, INSTALL_SUCCESS_MESSAGE, Paused, window_size


class ResponsiveUI(unittest.TestCase):
    dpi = 96

    def setUp(self):
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="responsive-ui-", dir=base)
        self.root = tk.Tk()
        self.root.withdraw()
        # Set font scaling before the app creates any widgets. Screen geometry
        # is an input fixture, not a dependency on the test machine's monitor.
        self.root.tk.call("tk", "scaling", self.dpi / 72)
        with patch.object(self.root, "winfo_screenwidth", return_value=1366), \
             patch.object(self.root, "winfo_screenheight", return_value=768), \
             patch.object(self.root, "winfo_fpixels", return_value=self.dpi):
            self.app = TextureQuestApp(self.root, self.temp.name, startup_check=False)

    def tearDown(self):
        for callback in self.root.tk.call("after", "info"):
            self.root.after_cancel(callback)
        self.root.destroy()
        self.temp.cleanup()

    def assert_inside_window(self, widget):
        self.assertTrue(widget.winfo_ismapped())
        left = widget.winfo_rootx() - self.root.winfo_rootx()
        top = widget.winfo_rooty() - self.root.winfo_rooty()
        self.assertGreaterEqual(left, 0)
        self.assertGreaterEqual(top, 0)
        self.assertLessEqual(left + widget.winfo_width(), self.root.winfo_width())
        self.assertLessEqual(top + widget.winfo_height(), self.root.winfo_height())

    def test_all_main_controls_fit_supported_displays_without_body_scrolling(self):
        # Font metrics, widget geometry and wrapping are handled by actual Tk.
        self.root.attributes("-alpha", 0)
        self.root.deiconify()
        self.app.busy = "update"
        self.app.pause_button.grid()
        self.app.update_check = {"installer": {"status": "verified", "version": "9.0.0",
            "update_available": True, "local_newer": False,
            "release_url": "https://github.com/Maergoth/TextureQuest/releases/tag/v9.0.0"}}
        self.app.refresh_controls()
        status = "Verifying C:/EverQuest II/paks/" + "long_archive_name_" * 12 + ".vpk; please wait for verification."
        self.app.events.put(("overall-progress", status,
                             14_000_000_000, 28_000_000_000, 155, 310))
        self.app.poll()
        self.assertFalse(hasattr(self.app, "body_canvas"))
        for screen in ((1366, 768), (1920, 1080)):
            width, height = window_size(*screen, self.dpi)
            with self.subTest(screen=screen, dpi=self.dpi, window=(width, height)):
                self.root.geometry(f"{width}x{height}")
                self.root.update()
                self.assertEqual((self.root.winfo_width(), self.root.winfo_height()), (width, height))
                for widget in (self.app.game_entry, self.app.download_button, self.app.install_button,
                               self.app.launch_button, self.app.menu_button, self.app.installer_update_button,
                               self.app.progress, self.app.overall_label, self.app.message, self.app.pause_button):
                    self.assert_inside_window(widget)
                self.assertEqual(float(self.app.progress.cget("value")), 50)
                self.assertIn("155/310 PAKs", self.app.overall_label.cget("text"))
                self.assertEqual(self.app.message.get("1.0", "end-1c"), status)

    def test_long_errors_are_read_only_and_scroll_without_moving_controls(self):
        self.root.attributes("-alpha", 0)
        self.root.deiconify()
        width, height = window_size(1366, 768, self.dpi)
        self.root.geometry(f"{width}x{height}")
        self.app.set_message("Ready")
        normal_color = self.app.message.cget("fg")
        error = "\n".join(f"Archive {n}: failed verification; retry the download." for n in range(30))
        self.app.set_message(error, error=True)
        self.root.update()
        self.assert_inside_window(self.app.progress)
        self.assert_inside_window(self.app.message)
        self.assertEqual(self.app.message.get("1.0", "end-1c"), error)
        self.assertEqual(self.app.message.cget("state"), "disabled")
        self.assertNotEqual(self.app.message.cget("fg"), normal_color)
        self.app.message.insert("1.0", "not editable")
        self.assertEqual(self.app.message.get("1.0", "end-1c"), error)
        controls_position = tuple((widget.winfo_rootx(), widget.winfo_rooty()) for widget in
                                  (self.app.game_entry, self.app.install_button, self.app.launch_button))
        self.app.message.event_generate("<MouseWheel>", delta=-120)
        self.root.update()
        self.assertGreater(self.app.message.yview()[0], 0)
        self.assertEqual(tuple((widget.winfo_rootx(), widget.winfo_rooty()) for widget in
                               (self.app.game_entry, self.app.install_button, self.app.launch_button)),
                         controls_position)

    def test_success_notice_is_exact_once_and_on_polling_thread(self):
        shown_on = []
        worker = threading.Thread(target=lambda: self.app.events.put(
            ("done", "install", {"status": "installed", "archives": []})))
        with patch.object(self.app, "show_install_success", side_effect=lambda: shown_on.append(threading.get_ident())) as notice:
            worker.start()
            worker.join()
            notice.assert_not_called()
            self.app.poll()
            self.app.poll()
            notice.assert_called_once_with()
        self.assertEqual(INSTALL_SUCCESS_MESSAGE, "Textures Installed. Launch via the EQ2 EXE, not the launcher!")
        self.assertEqual(shown_on, [threading.get_ident()])
        self.assertEqual(float(self.app.progress.cget("value")), 100)

    def test_failure_cancel_pause_and_other_completions_never_show_install_notice(self):
        self.app.recover_message = "Fixture installation failed"
        events = [
            ("error", "install", OSError("fixture failure")),
            ("cancel", "install", None),
            ("error", "update", Paused("fixture pause")),
            ("done", "verify", ({}, None)),
            ("done", "recover", ({}, None)),
            ("done", "prepare", ({"id": "fixture"}, {}, {"missing": []})),
            ("done", "update", (self.app.release, {"id": "fixture"}, {}, {"missing": []})),
        ]
        with patch.object(self.app, "show_install_success") as notice:
            for event in events:
                with self.subTest(event=event[:2]):
                    self.app.events.put(event)
                    self.app.poll()
                    notice.assert_not_called()


class Scale125UI(ResponsiveUI):
    dpi = 120


class Scale150UI(ResponsiveUI):
    dpi = 144


class WindowSizeTests(unittest.TestCase):
    def test_initial_client_size_leaves_dpi_scaled_screen_insets(self):
        for width, height, dpi in ((1366, 768, 96), (1366, 768, 120), (1366, 768, 144),
                                   (1920, 1080, 96), (1920, 1080, 120), (1920, 1080, 144)):
            with self.subTest(screen=(width, height, dpi)):
                w, h = window_size(width, height, dpi)
                self.assertLess(w, width)
                self.assertLess(h, height)
                self.assertGreater(w, 0)
                self.assertGreater(h, 0)


if __name__ == "__main__":
    unittest.main()
