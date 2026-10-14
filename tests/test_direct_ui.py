"""Direct delivery UI uses one cache and retains the explicit Install step."""
from pathlib import Path
import sys
import tempfile
import tkinter as tk
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from texture_swapper import core
import texturequest_app as app_module
from test_direct import fixture
import texturequest_direct as direct


class DirectDesktop(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1] / "build"
        root.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="direct-ui-", dir=root)
        self.root = tk.Tk()
        self.root.withdraw()
        self.manifest = fixture()[1]
        with patch.object(app_module, "load_manifest", return_value=self.manifest):
            self.app = app_module.TextureQuestApp(self.root, self.temp.name, startup_check=False)
        self.app.release_checked = True
        self.root.update_idletasks()

    def tearDown(self):
        for callback in self.root.tk.call("after", "info"):
            self.root.after_cancel(callback)
        self.root.destroy()
        self.temp.cleanup()

    def test_matching_saved_ready_survives_unavailable_update_without_installing(self):
        from test_release import vpk
        source = Path(self.temp.name) / 'own-input'
        source.mkdir()
        blobs, manifest = fixture()
        paths = []
        for name, blob in blobs.items():
            path = source / name
            path.write_bytes(blob)
            paths.append(path)
        saved = core.Swapper.prepare(self.app.core, paths)
        core._atomic(self.app.state_dir / 'texturequest-prepared.json', saved)
        original = direct.validate_manifest
        with patch.object(direct, 'validate_manifest', side_effect=lambda value,pinned=True: original(value,pinned=False)):
            self.app.release_checked = False
            unavailable = {'status':'unavailable','message':'Latest release still uses the older public format.'}
            with patch.object(self.app.core,'install',side_effect=AssertionError('must stay explicit')), patch.object(self.app,'begin'):
                self.app.events.put(('done','check',{'installer':unavailable,'pack':unavailable}))
                self.app.poll()
            self.assertEqual(self.app.prepared, saved)
            self.assertTrue(self.app.release_checked)
            self.assertTrue(all(Path(row['path']).exists() for row in saved['archives']))
            self.assertFalse((self.app.state_dir / 'downloads').exists())
            self.assertIn('unavailable', self.app.pack_update_label.cget('text'))

    def test_download_action_reuses_bundled_ready_on_old_server(self):
        source = Path(self.temp.name) / 'own-input'
        source.mkdir()
        blobs, _ = fixture()
        paths = []
        for name, blob in blobs.items():
            path = source / name
            path.write_bytes(blob)
            paths.append(path)
        saved = core.Swapper.prepare(self.app.core, paths)
        core._atomic(self.app.state_dir / 'texturequest-prepared.json', saved)
        original = direct.validate_manifest
        with patch.object(direct,'validate_manifest',side_effect=lambda value,pinned=True: original(value,pinned=False)), patch.object(app_module,'fetch_latest',side_effect=ValueError('older format')), patch.object(app_module,'prepare_direct',side_effect=AssertionError('no forced migration')), patch.object(self.app,'begin') as begin:
            self.app._download_latest()
            result = begin.call_args.args[2]()
        self.assertEqual(result[1], saved)
        self.assertFalse((self.app.state_dir / 'downloads').exists())

    def test_direct_download_checks_latest_without_any_zip_or_install(self):
        with patch.object(self.app, "begin") as begin, patch.object(app_module, "fetch_latest", return_value=self.manifest) as fetch, \
             patch.object(app_module, "prepare_direct", return_value={"schema": "texturequest-direct-prepared-v1"}) as prepare, \
             patch.object(self.app, "inspect", return_value=({}, None)), patch.object(self.app.core, "install") as install, \
             patch.object(app_module, "prepare_update") as legacy, patch.object(app_module, "download") as zip_download:
            self.app.download_pack()
            result = begin.call_args.args[2]()
            fetch.assert_called_once()
            prepare.assert_called_once()
            self.assertIs(prepare.call_args.args[1], self.manifest)
            self.assertIs(prepare.call_args.kwargs["cache_in_use"].__self__, self.app)
            self.assertIs(result[0], self.manifest)
            install.assert_not_called()
            legacy.assert_not_called()
            zip_download.assert_not_called()

    def test_completed_install_has_no_cleanup_question(self):
        rows = [dict(row, integrity="intact", actual_bytes=row["bytes"], actual_sha256=row["sha256"]) for row in self.manifest["archives"]]
        with patch.object(app_module.messagebox, "askyesno") as question:
            self.app.events.put(("done", "install", {"status": "installed", "archives": rows}))
            self.app.poll()
            question.assert_not_called()
        self.assertIn("ready", self.app.message.cget("text"))

    def test_pause_and_cache_guard_keep_install_disabled_during_preparation(self):
        self.app.busy = "update"
        self.app.refresh_controls()
        self.assertEqual(self.app.install_button.cget("state"), "disabled")
        self.app.request_pause()
        self.assertTrue(self.app.pause.is_set())
        self.app.cache_guard = lambda: True
        self.assertTrue(self.app.cache_in_use())
        with patch.object(app_module.messagebox, "askyesno") as question:
            self.app.clear_cache()
            question.assert_not_called()

    def test_sideload_remains_disabled_and_background_guard_never_queries_tk(self):
        self.assertEqual(self.app.sideload_checkbox.cget("state"), "disabled")
        self.app.sideload.set(True)
        with patch.object(self.app.sideload, "get", side_effect=RuntimeError("worker queried Tk")):
            self.assertTrue(self.app.cache_in_use())
        self.app.sideload.set(False)
        self.assertFalse(self.app.cache_in_use())


if __name__ == "__main__":
    unittest.main()
