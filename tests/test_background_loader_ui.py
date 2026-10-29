"""Installer mode UI: invented D folders and mocked loader/install leaves only."""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import threading
import tkinter as tk
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_app as module
from test_direct import fixture


class BackgroundLoaderUI(unittest.TestCase):
    dpi = 96

    def setUp(self):
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="background-ui-", dir=base)
        self.folder = Path(self.temp.name)
        self.game = self.folder / "game with spaces"
        self.game.mkdir()
        (self.game / "paks").mkdir()
        (self.game / "EverQuest2.exe").write_bytes(b"invented, never executed")
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.tk.call("tk", "scaling", self.dpi / 72)
        self.manager = MagicMock()
        self.state = dict(installed=False, running=False, active_sessions=0, available=True, message="")
        self.manager.status.side_effect = lambda game: dict(self.state)
        self.manager.cache_in_use.side_effect = lambda: bool(self.state["installed"] or self.state["active_sessions"])
        self.stack = ExitStack()
        self.stack.enter_context(patch.object(module, "LoaderManager", return_value=self.manager))
        self.stack.enter_context(patch.object(module, "load_manifest", return_value=fixture()[1]))
        with patch.object(self.root, "winfo_screenwidth", return_value=1366), \
             patch.object(self.root, "winfo_screenheight", return_value=768), \
             patch.object(self.root, "winfo_fpixels", return_value=self.dpi):
            self.app = module.TextureQuestApp(self.root, self.folder / "data", initial_game=self.game, startup_check=False)
        self.app.prepared = {"id": "invented-ready"}
        self.app.preflight = {"missing": [], "archives": [], "paks_dir": str(self.game / "paks")}
        self.app.release_checked = True
        self.app.refresh_controls()
        self.begin = self.stack.enter_context(patch.object(self.app, "begin"))

    def tearDown(self):
        self.stack.close()
        for callback in self.root.tk.call("after", "info"):
            self.root.after_cancel(callback)
        self.root.destroy()
        self.temp.cleanup()

    def message(self):
        return self.app.message.get("1.0", "end-1c")

    def select_background(self):
        self.app.mode_controls[1].invoke()
        self.assertEqual(self.app.install_mode.get(), "background")

    def run_action(self):
        return self.begin.call_args.args[2]()

    def test_radio_choices_are_exclusive_and_selection_has_no_install_or_cache_side_effect(self):
        self.assertEqual([w.cget("text") for w in self.app.mode_controls], ["Replace PAK files", "Background texture loader"])
        self.select_background()
        self.assertFalse(self.app.cache_in_use())
        self.assertEqual(self.app.install_button.cget("text"), "Install background loader")
        self.assertEqual(self.app.install_button.cget("bg"), module.ERROR)
        self.app.mode_controls[0].invoke()
        self.assertEqual(self.app.install_mode.get(), "pak")
        self.manager.install.assert_not_called()
        self.manager.uninstall.assert_not_called()
        self.begin.assert_not_called()

    def test_explicit_install_uses_worker_and_only_verified_status_turns_green(self):
        self.select_background()
        self.app.install_button.invoke()
        self.assertEqual(self.begin.call_args.args[0], "loader-install")
        self.manager.install.assert_not_called()
        self.app.core.status = MagicMock(return_value={"managed_count": 0, "archives": []})
        def install(game, prepared, progress):
            self.assertEqual(game, str(self.game))
            self.assertIs(prepared, self.app.prepared)
            self.state.update(installed=True, running=True, message="Installed. Current open session is unchanged; next launch uses the loader.")
        self.manager.install.side_effect = install
        result = self.run_action()
        self.app.events.put(("done", "loader-install", result))
        self.app.poll()
        self.manager.install.assert_called_once()
        self.assertEqual(self.app.install_button.cget("text"), "Uninstall background loader")
        self.assertEqual(self.app.install_button.cget("bg"), module.GREEN)
        self.assertIn("next launch", self.message())

    def test_loader_install_defers_actual_stock_validation_to_backend_despite_stale_managed_history(self):
        self.app.installed = {"managed_count": 310, "archives": [{"managed": True}]}
        self.select_background()
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget("state"), "normal")
        self.assertIn("official EQ2 patcher restores original files", self.app.install_label.cget("text"))
        self.assertIn("validates actual files", self.app.install_label.cget("text"))
        self.app.manage_loader()
        self.app.core.status = MagicMock(side_effect=AssertionError("UI metadata must not decide physical source compatibility"))
        self.manager.install.side_effect = ValueError("Actual game files need the official EQ2 patcher")
        with self.assertRaisesRegex(ValueError, "Actual game files"):
            self.run_action()
        self.manager.install.assert_called_once_with(str(self.game), self.app.prepared, progress=self.app.report)
        self.app.core.status.assert_not_called()
        self.assertEqual(self.app.install_button.cget("bg"), module.ERROR)

    def test_settings_removes_return_action_and_keeps_compatibility_method_uninvoked(self):
        labels = [self.app.menu.entrycget(i, "label") for i in range(self.app.menu.index("end") + 1)
                  if self.app.menu.type(i) != "separator"]
        self.assertNotIn("Return to original textures", labels)
        self.assertIn("Verify installed textures", labels)
        self.assertIn("Clear downloaded files & cache…", labels)
        self.assertTrue(callable(self.app.return_to_stock))
        self.assertEqual(self.app.menu.type(1), "separator")
        self.app.game.set("")
        self.app.refresh_controls()  # No stale menu index writes to separator.
        self.assertEqual(self.app.menu.entrycget(0, "state"), "disabled")
        self.manager.install.assert_not_called()
        self.manager.uninstall.assert_not_called()

    def test_pak_install_refuses_installed_running_or_existing_sessions(self):
        for state in (dict(installed=True), dict(running=True), dict(active_sessions=1)):
            self.state.update(installed=False, running=False, active_sessions=0)
            self.state.update(state)
            self.app.refresh_controls()
            self.assertEqual(self.app.install_button.cget("state"), "disabled")
            self.app.install()
            self.assertIn("Uninstall", self.message())
        self.begin.assert_not_called()

    def test_pak_worker_rechecks_loader_before_any_preflight_copy(self):
        self.app.install()
        self.assertEqual(self.begin.call_args.args[0], "install")
        self.state["installed"] = True
        with patch.object(self.app.core, "preflight") as preflight, self.assertRaisesRegex(RuntimeError, "Uninstall"):
            self.run_action()
        preflight.assert_not_called()

    def test_busy_disables_modes_and_actions_without_side_effects(self):
        self.select_background()
        for busy in ("install", "loader-install", "loader-uninstall", "app-update", "update"):
            self.app.busy = busy
            self.app.refresh_controls()
            self.assertTrue(all(w.cget("state") == "disabled" for w in self.app.mode_controls))
            self.assertEqual(self.app.install_button.cget("state"), "disabled")
            self.assertFalse(self.app.manage_loader())
        self.manager.install.assert_not_called()
        self.manager.uninstall.assert_not_called()

    def test_unavailable_invalid_and_exception_status_never_enable_or_show_green(self):
        self.select_background()
        for returned in (dict(self.state, available=False, message="Runtime not built"),
                         dict(self.state, installed="yes"), OSError("status failed")):
            if isinstance(returned, Exception):
                self.manager.status.side_effect = returned
            else:
                self.manager.status.side_effect = None
                self.manager.status.return_value = returned
            self.app.refresh_controls()
            self.assertEqual(self.app.install_button.cget("state"), "disabled")
            self.assertNotEqual(self.app.install_button.cget("bg"), module.GREEN)
            self.assertFalse(self.app.manage_loader())
        self.manager.install.assert_not_called()
        self.assertIn("status failed", self.message())

    def test_unconfirmed_install_result_is_an_error_not_fake_success(self):
        self.select_background()
        self.app.core.status = MagicMock(return_value={"managed_count": 0})
        self.app.manage_loader()
        with self.assertRaisesRegex(RuntimeError, "did not confirm"):
            self.run_action()
        self.assertEqual(self.app.install_button.cget("bg"), module.ERROR)

    def test_uninstall_does_not_claim_current_session_unhooked_and_prevents_reinstall(self):
        self.state.update(installed=True, running=True, active_sessions=1)
        self.select_background()
        self.assertTrue(self.app.manage_loader())
        self.assertEqual(self.begin.call_args.args[0], "loader-uninstall")
        def uninstall(game, progress):
            self.state.update(installed=False, running=False,
                              message="Uninstalled for future launches. Current session keeps textures until exit.")
        self.manager.uninstall.side_effect = uninstall
        result = self.run_action()
        self.app.events.put(("done", "loader-uninstall", result))
        self.app.poll()
        self.assertIn("Current session", self.message())
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
        self.assertTrue(self.app.cache_in_use())
        self.app.mode_controls[0].invoke()
        self.assertEqual(self.app.install_button.cget("state"), "disabled")

    def test_cache_and_app_update_refuse_live_loader_and_worker_cache_guard_never_reads_tk(self):
        self.state["installed"] = True
        self.app.update_check = {"installer": dict(status="verified", update_available=True, version="9.0.0")}
        self.app.refresh_controls()
        self.assertEqual(self.app.installer_update_button.cget("state"), "normal")
        with patch.object(module.messagebox, "askyesno") as question:
            self.app.clear_cache()
            question.assert_not_called()
        self.assertFalse(self.app.open_app_update())
        results = []
        with patch.object(self.app.install_mode, "get", side_effect=AssertionError("worker queried Tk")):
            worker = threading.Thread(target=lambda: results.append(self.app.cache_in_use()))
            worker.start()
            worker.join()
        self.assertEqual(results, [True])
        self.begin.assert_not_called()

    def test_constructor_failure_is_honest_and_blocks_conflicting_actions(self):
        self.app.loader_manager = None
        self.app.loader_error = "Background loader unavailable: fixture constructor failure"
        self.select_background()
        self.assertIn("constructor failure", self.app.install_label.cget("text"))
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
        self.app.mode_controls[0].invoke()
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
        self.assertTrue(self.app.cache_in_use())

    def test_real_constructor_error_and_absent_optional_module_have_distinct_guards(self):
        for factory, error, blocked in ((None, "", False), (MagicMock(side_effect=OSError("constructor refused")), "", True),
                                        (None, "Background loader unavailable: internal import refused", True)):
            other_root = tk.Tk()
            other_root.withdraw()
            try:
                with patch.object(module, "LoaderManager", factory), patch.object(module, "LOADER_IMPORT_ERROR", error):
                    app = module.TextureQuestApp(other_root, self.folder / "constructor-data", initial_game=self.game, startup_check=False)
                app.prepared, app.preflight, app.release_checked = self.app.prepared, self.app.preflight, True
                app.refresh_controls()
                self.assertEqual(app.install_button.cget("state"), "disabled" if blocked else "normal")
                self.assertEqual(app.cache_in_use(), blocked)
                app.install_mode.set("background")
                self.assertEqual(app.install_button.cget("state"), "disabled")
                self.assertIn("unavailable" if blocked else "not included", app.install_label.cget("text"))
            finally:
                for callback in other_root.tk.call("after", "info"):
                    other_root.after_cancel(callback)
                other_root.destroy()

    def test_unavailable_staged_runtime_allows_pak_mode_but_status_error_blocks_cache_and_update(self):
        self.state["available"] = False
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget("state"), "normal")
        self.assertFalse(self.app.cache_in_use())
        self.manager.status.side_effect = OSError("unknown ownership")
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
        self.assertTrue(self.app.cache_in_use())
        with patch.object(module.messagebox, "askyesno") as question:
            self.app.clear_cache()
            question.assert_not_called()
        self.assertFalse(self.app.open_app_update())

    def test_loader_for_another_game_folder_blocks_conflicting_buttons_and_explains_selection(self):
        self.manager.cache_in_use.side_effect = lambda: True
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
        self.select_background()
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
        self.assertIn("installed game folder", self.app.install_label.cget("text"))
        self.manager.install.assert_not_called()
        self.manager.uninstall.assert_not_called()

    def test_modes_and_actions_fit_compact_window_without_body_scroll(self):
        self.select_background()
        self.root.attributes("-alpha", 0)
        self.root.deiconify()
        width, height = module.window_size(1366, 768, self.dpi)
        self.root.geometry(f"{width}x{height}")
        self.root.update()
        for widget in (*self.app.mode_controls, self.app.install_button, self.app.launch_button):
            self.assertTrue(widget.winfo_ismapped())
            self.assertGreaterEqual(widget.winfo_rooty(), self.root.winfo_rooty())
            self.assertLessEqual(widget.winfo_rooty() + widget.winfo_height(), self.root.winfo_rooty() + self.root.winfo_height())
        self.assertFalse(hasattr(self.app, "body_canvas"))


class BackgroundLoaderHighDPIUI(unittest.TestCase):
    dpi = 144
    setUp = BackgroundLoaderUI.setUp
    tearDown = BackgroundLoaderUI.tearDown
    select_background = BackgroundLoaderUI.select_background
    test_background_controls_fit_150_percent = BackgroundLoaderUI.test_modes_and_actions_fit_compact_window_without_body_scroll


if __name__ == "__main__":
    unittest.main()
