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
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="responsive-ui-", dir=base)
        self.root = tk.Tk()
        self.root.withdraw()
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

    def test_totals_and_status_stay_visible_at_both_scroll_extremes(self):
        # Font metrics, widget geometry and wrapping are handled by actual Tk.
        self.root.attributes("-alpha", 0)
        self.root.deiconify()
        self.app.busy = "update"
        self.app.pause_button.grid()
        status = "Verifying C:/EverQuest II/paks/" + "long_archive_name_" * 12 + ".vpk; please wait for verification."
        self.app.events.put(("overall-progress", status,
                             14_000_000_000, 28_000_000_000, 155, 310))
        self.app.poll()
        for width, height in ((840, 640), (640, 480), (640, 360)):
            with self.subTest(window=(width, height)):
                self.root.geometry(f"{width}x{height}")
                self.root.update()
                self.assertEqual((self.root.winfo_width(), self.root.winfo_height()), (width, height))
                self.assertGreater(float(self.app.body_canvas.cget("scrollregion").split()[3]),
                                   self.app.body_canvas.winfo_height())
                fixed = None
                for fraction in (0, 1):
                    self.app.body_canvas.yview_moveto(fraction)
                    self.root.update()
                    for widget in (self.app.progress, self.app.overall_label, self.app.message, self.app.pause_button):
                        self.assert_inside_window(widget)
                    geometry = (self.app.progress.winfo_rooty(), self.app.overall_label.winfo_rooty(),
                                self.app.message.winfo_rooty())
                    if fixed is not None:
                        self.assertEqual(geometry, fixed)
                    fixed = geometry
                self.assertEqual(float(self.app.progress.cget("value")), 50)
                self.assertIn("155/310 PAKs", self.app.overall_label.cget("text"))
                self.assertEqual(self.app.message.get("1.0", "end-1c"), status)

    def test_long_errors_are_read_only_and_scroll_without_moving_cards(self):
        self.root.attributes("-alpha", 0)
        self.root.deiconify()
        self.root.geometry("640x360")
        error = "\n".join(f"Archive {n}: failed verification; retry the download." for n in range(30))
        self.app.set_message(error, error=True)
        self.root.update()
        self.assert_inside_window(self.app.progress)
        self.assert_inside_window(self.app.message)
        self.assertEqual(self.app.message.get("1.0", "end-1c"), error)
        self.assertEqual(self.app.message.cget("state"), "disabled")
        self.assertEqual(self.app.message.cget("fg"), "#9a492c")
        self.app.message.insert("1.0", "not editable")
        self.assertEqual(self.app.message.get("1.0", "end-1c"), error)
        cards_position = self.app.body_canvas.yview()
        self.app.message.event_generate("<MouseWheel>", delta=-120)
        self.root.update()
        self.assertGreater(self.app.message.yview()[0], 0)
        self.assertEqual(self.app.body_canvas.yview(), cards_position)

    def test_success_notice_is_exact_once_and_on_polling_thread(self):
        shown_on = []
        worker = threading.Thread(target=lambda: self.app.events.put(
            ("done", "install", {"status": "installed", "archives": []})))
        with patch("texturequest_app.messagebox.showinfo", side_effect=lambda *a, **k: shown_on.append(threading.get_ident())) as notice:
            worker.start()
            worker.join()
            notice.assert_not_called()
            self.app.poll()
            self.app.poll()
            notice.assert_called_once_with("TextureQuest", "Textures Installed. Launch via the EQ2 EXE, not the launcher!", parent=self.root)
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
        with patch("texturequest_app.messagebox.showinfo") as notice:
            for event in events:
                with self.subTest(event=event[:2]):
                    self.app.events.put(event)
                    self.app.poll()
                    notice.assert_not_called()


class ScaledResponsiveUI(ResponsiveUI):
    # Each interpreter gets its own point-to-pixel scale before font creation.
    scale = 96 / 72

    def setUp(self):
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="scaled-ui-", dir=base)
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.tk.call("tk", "scaling", self.scale)
        self.app = TextureQuestApp(self.root, self.temp.name, startup_check=False)


class Scale150UI(ScaledResponsiveUI):
    scale = 144 / 72


class Scale200UI(ScaledResponsiveUI):
    scale = 192 / 72


class WindowSizeTests(unittest.TestCase):
    def test_initial_client_size_leaves_dpi_scaled_screen_insets(self):
        for width, height, dpi in ((1920, 1080, 96), (1024, 600, 144), (800, 480, 192), (600, 320, 96)):
            with self.subTest(screen=(width, height, dpi)):
                w, h = window_size(width, height, dpi)
                self.assertLess(w, width)
                self.assertLess(h, height)
                self.assertGreater(w, 0)
                self.assertGreater(h, 0)


if __name__ == "__main__":
    unittest.main()
