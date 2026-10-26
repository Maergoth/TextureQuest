"""End-user UI state tests; no network, no real game, no visible windows."""
import copy
from pathlib import Path
import sys
import tempfile
import tkinter as tk
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from texturequest_app import TextureQuestApp, can_install, installation_current, download_space_needed, game_space_needed
from texture_swapper import core


def ui_manifest(texture="1.1.0"):
    """Small direct manifest, independent of the current shipping manifest shape."""
    from test_direct import fixture
    manifest = fixture()[1]
    manifest["texture_version"] = texture
    return manifest


class StateTests(unittest.TestCase):
    def test_complete_current_bytes_required(self):
        manifest = {"archives": [{"name": "a.vpk", "bytes": 20, "sha256": "abc"}]}
        row = {"name": "a.vpk", "integrity": "intact", "actual_bytes": 20, "actual_sha256": "abc"}
        self.assertTrue(installation_current({"archives": [row]}, manifest))
        for key, value in (("integrity", "changed"), ("actual_bytes", 19), ("actual_sha256", "old")):
            changed = {**row, key: value}
            self.assertFalse(installation_current({"archives": [changed]}, manifest))
        self.assertFalse(installation_current({"status": "installed", "archives": []}, manifest))
    def test_install_is_explicit_and_requires_full_preflight(self):
        self.assertTrue(can_install(None, {"id": "ready"}, {"missing": []}))
        self.assertFalse(can_install("download", {"id": "ready"}, {"missing": []}))
        self.assertFalse(can_install(None, None, {"missing": []}))
        self.assertFalse(can_install(None, {"id": "ready"}, {"missing": ["a.vpk"]}))
    def test_download_space_counts_remaining_zip_and_prepared_files(self):
        reserve = 64 * 1024**2
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as folder:
            folder = Path(folder)
            manifest = {"archive": {"bytes": 100}, "archives": [{"bytes": 150}]}
            self.assertEqual(download_space_needed(manifest, folder), 250 + reserve)
            (folder / "Texture-Pack.zip.download").write_bytes(b"x" * 30)
            self.assertEqual(download_space_needed(manifest, folder), 220 + reserve)
            self.assertEqual(download_space_needed(manifest, folder, True), 70 + reserve)
    def test_game_space_reserves_growth_and_one_archive(self):
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as folder:
            path = Path(folder) / "fixture.vpk"
            path.write_bytes(b"x" * 10)
            self.assertEqual(game_space_needed({"archives": [{"target": str(path), "bytes": 30}]}), 50 + 64 * 1024**2)


class DesktopTests(unittest.TestCase):
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(prefix="ui-", dir=base)
        self.root = tk.Tk()
        self.root.withdraw()
        self.app = TextureQuestApp(self.root, self.tmp.name)
        self.app.release_checked = True  # Offline tests supply a verified release.
        self.root.update_idletasks()
    def tearDown(self):
        for callback in self.root.tk.call("after", "info"):
            self.root.after_cancel(callback)
        self.root.destroy()
        self.tmp.cleanup()
    def test_clean_start_and_three_step_consumer_controls(self):
        self.assertEqual(self.app.game.get(), "")
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
        self.assertIn(self.app.download_button.cget("text"), ("Download pack", "Download / Resume"))
        self.assertFalse(hasattr(self.app, "tree"))
        self.assertFalse(hasattr(self.app, "explorer_button"))
        self.assertFalse(self.app.menu.winfo_ismapped())
    def test_game_edit_invalidates_previous_install_target(self):
        self.app.prepared = {"id": "ready"}
        self.app.preflight = {"missing": []}
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget("state"), "normal")
        self.app.game.set("different-folder")
        self.assertIsNone(self.app.preflight)
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
    def test_missing_archive_never_enables_install(self):
        self.app.prepared = {"id": "ready"}
        self.app.preflight = {"missing": ["a.vpk"]}
        self.app.refresh_controls()
        with patch.object(self.app.core, "install") as install:
            self.app.install()
            install.assert_not_called()
    def test_prepared_event_does_not_install_automatically(self):
        with patch.object(self.app.core, "install") as install:
            self.app.events.put(("done", "prepare", ({"id": "ready"}, {}, {"missing": []})))
            self.app.poll()
            install.assert_not_called()
            self.assertEqual(self.app.install_button.cget("state"), "normal")
    def test_pause_control_only_affects_download(self):
        self.app.busy = "install"
        self.app.request_pause()
        self.assertFalse(self.app.pause.is_set())
        self.app.busy = "download"
        self.app.request_pause()
        self.assertTrue(self.app.pause.is_set())
    def test_optional_external_zip_checks_latest_before_preparation(self):
        saved = Path(self.tmp.name) / "outside-download-cache.zip"
        saved.write_bytes(b"fixture")
        from test_release import fixture
        manifest = fixture()[2]
        with patch.object(self.app, "begin") as begin, patch("texturequest_app.fetch_latest", return_value=manifest) as latest, \
             patch("texturequest_app.prepare_release", return_value={"id": "ready"}) as prepare, \
             patch("texturequest_app.download") as network, patch.object(self.app, "inspect", return_value=({}, None)):
            self.app.prepare_zip(saved)
            begin.call_args.args[2]()
            latest.assert_called_once()
            prepare.assert_called_once_with(self.app.core, saved, manifest, self.app.report)
            network.assert_not_called()
    def test_restart_finds_prior_selected_zip(self):
        saved = Path(self.tmp.name) / "retained.zip"
        saved.write_bytes(b"fixture")
        core._save(self.app.state_dir / "texturequest-prepared.json", {"source_paths": [str(saved)]})
        self.assertEqual(self.app.saved_zip_path(), saved)
        self.app.refresh_controls()
        expected = "Download / Resume" if self.app.release.get("schema") in ("texturequest-release-v2", "texturequest-release-v3") else "Prepare saved pack"
        self.assertEqual(self.app.download_button.cget("text"), expected)
    def test_compact_ui_has_settings_and_launch_without_body_scroll_or_sideload(self):
        self.root.update_idletasks()
        self.assertFalse(hasattr(self.app, "body_canvas"))
        self.assertFalse(hasattr(self.app, "sideload_checkbox"))
        self.assertTrue(self.app.menu_button.cget("text").startswith("Settings"))
        self.assertEqual(self.app.launch_button.cget("text"), "Launch Game")
        widgets, texts = [self.root], []
        while widgets:
            widget = widgets.pop()
            widgets.extend(widget.winfo_children())
            if "text" in widget.keys():
                texts.append(widget.cget("text"))
        self.assertNotIn("After a game patch, rerun this installer. Saved textures are reused.", texts)
        self.assertFalse(any("Maintenance & updates" in str(text) for text in texts))

    def test_app_only_update_retains_prepared_textures_and_shows_official_action(self):
        from texturequest_version_check import pack_identity
        self.app.release = ui_manifest()
        current = self.app.release
        same = copy.deepcopy(current); same["version"] = "1.2.1"
        self.app.prepared = {"id": "ready", "archives": copy.deepcopy(current["archives"])}
        self.app.preflight = {"missing": []}
        result = {"installer": {"status": "verified", "version": "9.0.0", "update_available": True,
                               "local_newer": False, "release_url": "https://github.com/Maergoth/TextureQuest/releases/tag/v9.0.0"},
                  "pack": {"status": "verified", "version": "1.1.0", "manifest": same,
                           "identity": pack_identity(same), "update_available": False}}
        self.app.events.put(("done", "check", result))
        with patch.object(self.app.core, "install") as install:
            self.app.poll()
            install.assert_not_called()
        self.assertIsNotNone(self.app.prepared)
        self.assertEqual(self.app.install_button.cget("state"), "normal")
        self.assertIn("App update available", self.app.app_update_label.cget("text"))
        self.assertIn("Latest textures: 1.1.0", self.app.pack_update_label.cget("text"))
        with patch.object(sys, "frozen", False, create=True), patch.object(self.app, "begin") as begin:
            self.assertFalse(self.app.open_app_update())
            begin.assert_not_called()
        self.assertIn("distributed TextureQuest app", self.app.message.get("1.0", "end-1c"))

    def test_offline_check_keeps_selection_and_disables_false_current_install(self):
        current = self.app.release
        self.app.prepared = {"id": "ready"}; self.app.preflight = {"missing": []}
        with patch.object(self.app, "begin") as begin:
            self.app.check_updates()
            self.assertEqual(begin.call_args.args[0], "check")
        unavailable = {"status": "unavailable", "message": "Could not reach GitHub."}
        self.app.events.put(("done", "check", {"installer": unavailable, "pack": unavailable}))
        self.app.poll()
        self.assertIs(self.app.release, current)
        self.assertIsNotNone(self.app.prepared)
        self.assertFalse(self.app.release_checked)
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
        self.assertIn("unavailable", self.app.app_update_label.cget("text"))
        self.assertIn("unavailable", self.app.pack_update_label.cget("text"))
        with patch.object(self.app, "begin") as begin:
            self.assertFalse(self.app.open_app_update())
            begin.assert_not_called()

    def test_app_check_failure_keeps_pack_update_downloadable(self):
        manifest = ui_manifest("1.2.0")
        result = {"installer": {"status": "unavailable", "message": "No unique installer"},
                  "pack": {"status": "verified", "version": "1.2.0", "manifest": manifest, "update_available": True}}
        self.app.prepared = {"id": "old"}; self.app.preflight = {"missing": []}
        self.app.events.put(("done", "check", result)); self.app.poll()
        self.assertIsNone(self.app.prepared)
        self.assertTrue(self.app.release_checked)
        self.assertIn("New textures available: 1.2.0", self.app.pack_update_label.cget("text"))
        self.assertEqual(self.app.download_button.cget("state"), "normal")
        self.assertEqual(self.app.install_button.cget("state"), "disabled")

    def test_header_reports_application_build_version_separately(self):
        from texturequest_version_check import texture_version
        self.assertEqual(self.app.version_label.cget("text"), "App " + self.app.app_version)
        self.assertIn("Textures " + texture_version(self.app.release), self.app.pack_versions_label.cget("text"))

    def test_startup_and_manual_check_use_both_version_channels(self):
        manifest = ui_manifest()
        self.app.release = manifest
        with patch.object(self.app, "begin") as begin, patch("texturequest_app.check_versions", return_value="both") as check:
            self.root.after(300, self.root.quit)
            self.root.mainloop()
            begin.assert_called_once()
            self.assertEqual(begin.call_args.args[0], "check")
            self.assertEqual(begin.call_args.args[2](), "both")
            self.app.check_updates()
            self.assertEqual(begin.call_args.args[2](), "both")
            self.assertEqual(check.call_count, 2)
            check.assert_called_with(manifest, self.app.app_version, self.app.state_dir)

    def test_sideload_is_disabled_and_never_routes_to_install(self):
        self.assertFalse(hasattr(self.app, "sideload_checkbox"))
        self.assertFalse(self.app.sideload.get())
        self.app.prepared = {"id": "ready"}
        self.app.preflight = {"missing": []}
        self.app.sideload.set(True)
        with patch.object(self.app.core, "install") as install, patch.object(self.app, "begin") as begin:
            self.app.install()
            install.assert_not_called()
            begin.assert_not_called()

    def test_update_preparation_routes_without_automatic_install(self):
        from test_updates import update_fixture
        self.app.release = update_fixture()[0]
        self.app.refresh_controls()
        self.assertEqual(self.app.download_button.cget("text"), "Download / Resume")
        with patch.object(self.app, "download_update") as update:
            self.app.download_pack()
            update.assert_called_once_with()
        with patch.object(self.app.core, "install") as install:
            self.app.events.put(("done", "update", (self.app.release, {"id": "ready"}, {}, {"missing": []})))
            self.app.poll()
            install.assert_not_called()
            self.assertEqual(self.app.install_button.cget("state"), "normal")

    def test_latest_check_invalidates_install_until_verified(self):
        self.app.prepared = {"id": "ready"}
        self.app.preflight = {"missing": []}
        with patch.object(self.app, "begin") as begin:
            self.app.check_updates()
            begin.assert_called_once()
        self.app.refresh_controls()
        self.assertFalse(self.app.release_checked)
        self.assertEqual(self.app.install_button.cget("state"), "disabled")

    def test_download_checks_latest_and_requires_no_user_supplied_zip(self):
        from test_updates import update_fixture
        latest = update_fixture()[0]
        with patch.object(self.app, "begin") as begin, patch("texturequest_app.fetch_latest", return_value=latest) as fetch, \
             patch("texturequest_app.prepare_update", return_value={"id": "ready"}) as prepare, \
             patch.object(self.app, "inspect", return_value=({}, None)), patch.object(self.app.core, "install") as install:
            self.app.download_pack()
            result = begin.call_args.args[2]()
            fetch.assert_called_once()
            self.assertIs(prepare.call_args.args[1], latest)
            self.assertIsNone(prepare.call_args.args[3])
            self.assertEqual(result[0], latest)
            install.assert_not_called()

    def test_maintenance_has_no_protection_or_user_zip_step(self):
        labels = [self.app.menu.entrycget(i, "label") for i in range(self.app.menu.index("end") + 1)
                  if self.app.menu.type(i) != "separator"]
        self.assertTrue({"Verify installed textures", "Return to original textures",
                         "Clear downloaded files & cache…", "Check for updates", "Instructions"}.issubset(labels))
        self.assertFalse(any("protection" in label.lower() or "prepare for" in label.lower() or "zip" in label.lower() for label in labels))

    def test_update_pause_and_warning_text(self):
        from texturequest_app import SIDELOAD_WARNING
        self.assertEqual(SIDELOAD_WARNING, "We don't know what's more legal, replacing the game files or hooking the process. If Daybreak hates you, they might ban you. Be warned.")
        self.app.busy = "update"
        self.app.request_pause()
        self.assertTrue(self.app.pause.is_set())


if __name__ == "__main__":
    unittest.main()
