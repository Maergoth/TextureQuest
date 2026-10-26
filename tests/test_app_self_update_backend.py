"""New updater backend tests: D-only files, fake network and process leaves."""
from contextlib import contextmanager
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_self_update as update


def raw(value):
    return json.dumps(value, sort_keys=True).encode()


def bundle(version, marker):
    release = raw(dict(schema="texturequest-release-v3", version="1.3.7", texture_version="1.2.3", archives=[]))
    files = {"TextureQuest.exe": b"MZ" + marker * 100,
             "_internal/release-manifest.json": release, "_internal/runtime.dll": marker * 20,
             "_internal/empty.dat": b"", "licenses/LICENSE.txt": b"license",
             "START-HERE.txt": b"instructions " + marker}
    manifest = dict(application="TextureQuest", version=version, texture_version="1.2.3", manifest_version="1.3.7",
                    runtime="CPython fixture", dependencies={}, source_sha256={"src/texturequest_self_update.py": "b" * 64},
                    release_manifest_sha256=hashlib.sha256(release).hexdigest(),
                    files=[dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()) for name, data in files.items()])
    return dict(files, **{"BUILD-MANIFEST.json": raw(manifest)})


def make_bundle(root, files):
    root.mkdir()
    for name, data in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def make_zip(path, files):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as output:
        for name, data in files.items():
            output.writestr("TextureQuest/" + name, data)
    data = path.read_bytes()
    return dict(status="verified", version="1.3.6", bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                release_url="https://github.com/Maergoth/TextureQuest/releases/tag/v1.3.7",
                download_url="https://github.com/Maergoth/TextureQuest/releases/download/v1.3.7/TextureQuest-Installer-1.3.6-windows-x64.zip")


class Processes:
    def __init__(self, app):
        self.app = app
        self.old = dict(pid=101, birth=100.25, exe=str(app / "TextureQuest.exe"))
        self.current = self.old
        self.living = {101: self.old}
        self.events, self.spawned, self.stopped = [], [], []
        self.wait_success, self.refuse_other, self.auto_ack = True, False, True
        self.on_spawn = None
        self.child = None
        self.request = None
        self.helper = None

    def identity(self, pid=None):
        return self.current if pid is None else self.living[pid]

    def alive(self, identity):
        if self.child and identity == self.child and self.auto_ack and self.request:
            path = Path(self.request["request_path"])
            if (path.parent / "launched.json").exists() and not (path.parent / "startup-ack.json").exists():
                prior, self.current = self.current, self.child
                try:
                    update.acknowledge_startup(path, self.request["nonce"], app_dir=self.app,
                                               data_dir=self.app / "data", version="1.3.6", processes=self)
                finally:
                    self.current = prior
        return self.living.get(identity["pid"]) == identity

    def other_instances(self, app, allowed=()):
        self.events.append(("instances", str(app)))
        if self.refuse_other:
            raise update.AppUpdateError("Other instance")

    @contextmanager
    def parent(self, identity):
        if self.living.get(identity["pid"]) != identity:
            raise update.AppUpdateError("Wrong parent identity")
        def wait(seconds):
            self.events.append(("wait", seconds))
            if self.wait_success:
                self.living.pop(identity["pid"], None)
            return self.wait_success
        yield wait

    def spawn(self, executable, args, cwd):
        child = dict(pid=200 + len(self.spawned), birth=200.25 + len(self.spawned), exe=str(executable))
        self.living[child["pid"]] = child
        self.spawned.append((child, list(args), str(cwd)))
        if args[0] == "--app-update-ack":
            self.child = child
        if self.on_spawn:
            self.on_spawn(child, args)
        return child

    def stop_owned(self, child, helper):
        if helper != self.current or child != self.child:
            raise AssertionError("Foreign stop")
        self.stopped.append(child)
        self.living.pop(child["pid"], None)

    def as_helper(self, request):
        self.request = request
        self.helper = dict(pid=150, birth=150.25, exe=str(Path(request["request_path"]).parent / "payload/TextureQuest/TextureQuest.exe"))
        self.living[150] = self.helper
        self.current = self.helper


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent, prefix="self-update-backend-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / "app"
        self.old = bundle("1.3.5", b"O")
        self.new = bundle("1.3.6", b"N")
        make_bundle(self.app, self.old)
        (self.app / "data").mkdir()
        self.data = self.app / "data/large-texture.marker"
        self.data.write_bytes(b"user textures, receipts, selections")
        self.unknown = self.app / "personal-note.txt"
        self.unknown.write_bytes(b"must not be touched")
        self.before = {str(p): (p.stat().st_ino, p.read_bytes()) for p in (self.data, self.unknown)}
        self.archive = self.root / "installer.zip"
        self.installer = make_zip(self.archive, self.new)
        self.processes = Processes(self.app)

    def stage(self):
        return update.stage_downloaded_update(self.installer, self.archive, app_dir=self.app, data_dir=self.app / "data",
                                              current_version="1.3.5", processes=self.processes)

    def assert_preserved(self):
        for name, value in self.before.items():
            path = Path(name)
            self.assertEqual((path.stat().st_ino, path.read_bytes()), value)

    def run_apply(self, request):
        self.processes.as_helper(request)
        with patch.object(update, "ACK_SECONDS", 0.2):
            return update.apply_update(request["request_path"], request["nonce"], processes=self.processes)

    def test_staging_checks_all_files_and_preserves_data_unknown_and_zero_byte_file(self):
        walked = []
        original = update.os.walk
        def walk(path, **kwargs):
            walked.append(Path(path))
            return original(path, **kwargs)
        with patch.object(update.os, "walk", side_effect=walk):
            request = self.stage()
        self.assertTrue(Path(request["request_path"]).is_file())
        self.assertFalse(any(p == self.app or self.app / "data" in p.parents or p == self.app / "data" for p in walked))
        self.assert_preserved()
        self.assertEqual((self.app / "TextureQuest.exe").read_bytes(), self.old["TextureQuest.exe"])
        self.assertEqual(update._request(request["request_path"], request["nonce"])[0]["target_version"], "1.3.6")

    def test_checksum_and_official_identity_refuse_before_stage(self):
        self.archive.write_bytes(self.archive.read_bytes() + b"changed")
        with self.assertRaisesRegex(update.AppUpdateError, "checksum"):
            self.stage()
        self.assertFalse(list(self.root.glob(".TextureQuest-update-*")))
        with self.assertRaises(update.AppUpdateError):
            update._installer(dict(self.installer, download_url="https://example.org/installer.zip"), "1.3.5")
        self.assert_preserved()

    def test_custom_data_and_other_instance_refuse(self):
        with self.assertRaisesRegex(update.AppUpdateError, "Custom"):
            update.stage_downloaded_update(self.installer, self.archive, app_dir=self.app, data_dir=self.root / "custom-data",
                                           current_version="1.3.5", processes=self.processes)
        self.processes.refuse_other = True
        with self.assertRaisesRegex(update.AppUpdateError, "Other instance"):
            self.stage()
        self.assert_preserved()

    def test_zip_unsafe_paths_duplicates_symlinks_and_data_payload_refuse(self):
        names = ["../escape", "_internal/../../escape", "_internal\\escape", "data/precious.vpk", "CON.txt",
                 "_internal/trailing. ", "_internal/drive:evil", "_internal/COM1.dll"]
        for index, name in enumerate(names):
            with self.subTest(name=name):
                path = self.root / ("bad" + str(index) + ".zip")
                make_zip(path, dict(self.new, **{name: b"bad"}))
                with self.assertRaises(update.AppUpdateError):
                    update._extract(path, self.root / ("bad-out" + str(index)), "1.3.6")
        for kind in ("alias", "symlink", "duplicate"):
            path = self.root / (kind + ".zip")
            make_zip(path, self.new)
            with zipfile.ZipFile(path, "a") as output:
                info = zipfile.ZipInfo("TextureQuest/_internal/RUNTIME.dll" if kind == "alias" else "TextureQuest/_internal/link")
                if kind == "symlink":
                    info.create_system = 3
                    info.external_attr = (0o120777 << 16)
                if kind == "duplicate":
                    info.filename = "TextureQuest/TextureQuest.exe"
                output.writestr(info, b"bad")
            with self.assertRaises(update.AppUpdateError):
                update._extract(path, self.root / (kind + "-out"), "1.3.6")
        self.assert_preserved()

    def test_zip_inventory_hash_version_and_expansion_bounds_refuse(self):
        variants = [dict(self.new, **{"_internal/runtime.dll": b"changed"}),
                    dict(self.new, **{"_internal/unlisted.dll": b"unexpected"})]
        manifest = json.loads(self.new["BUILD-MANIFEST.json"])
        manifest["version"] = "1.3.7"
        variants.append(dict(self.new, **{"BUILD-MANIFEST.json": raw(manifest)}))
        for index, files in enumerate(variants):
            path = self.root / ("invalid" + str(index) + ".zip")
            make_zip(path, files)
            with self.assertRaises(update.AppUpdateError):
                update._extract(path, self.root / ("invalid-out" + str(index)), "1.3.6")
        with patch.object(update, "MAX_EXPANDED", 128):
            with self.assertRaisesRegex(update.AppUpdateError, "limits"):
                update._extract(self.archive, self.root / "too-large", "1.3.6")

    def test_real_filesystem_commit_waits_parent_and_matching_startup_ack(self):
        request = self.stage()
        self.assertEqual(self.run_apply(request), 0)
        self.assertEqual((self.app / "TextureQuest.exe").read_bytes(), self.new["TextureQuest.exe"])
        stage = Path(request["request_path"]).parent
        result = update._load(stage / "result.json")
        self.assertEqual(result["status"], "committed")
        self.assertEqual(update._load(stage / "startup-ack.json")["child"], self.processes.child)
        self.assertIn(("wait", update.PARENT_SECONDS), self.processes.events)
        self.assertEqual((stage / "backup/TextureQuest.exe").read_bytes(), self.old["TextureQuest.exe"])
        self.assertEqual(self.processes.spawned[0][1][0], "--app-update-ack")
        self.assertFalse(self.processes.stopped)
        self.assert_preserved()

    def test_partial_rename_failure_restores_original_app_without_data_traversal(self):
        request = self.stage()
        move, calls = update._move, []
        def failing(source, destination):
            calls.append((source, destination))
            if len(calls) == 3:
                raise OSError("Own fixture held application file")
            return move(source, destination)
        with patch.object(update, "_move", side_effect=failing):
            self.assertEqual(self.run_apply(request), 2)
        self.assertEqual(update._bundle(self.app, "1.3.5"), update._request(request["request_path"], request["nonce"])[0]["old_bundle"])
        result = update._load(Path(request["request_path"]).parent / "result.json")
        self.assertEqual(result["rollback"], "restored")
        self.assertIn("held", result["message"])
        self.assertEqual(self.processes.spawned[-1][1][0], "--app-update-failed")
        self.assert_preserved()

    def test_startup_ack_failure_rolls_back_and_stops_only_own_new_child(self):
        request = self.stage()
        self.processes.auto_ack = False
        self.assertEqual(self.run_apply(request), 2)
        self.assertEqual(self.processes.stopped, [self.processes.child])
        self.assertEqual((self.app / "TextureQuest.exe").read_bytes(), self.old["TextureQuest.exe"])
        self.assertIn("previous app was restored", update.failure_message(request["request_path"], request["nonce"]))
        self.assert_preserved()

    def test_wrong_parent_and_parent_timeout_never_replace_files(self):
        request = self.stage()
        self.processes.living[101] = dict(self.processes.old, birth=999)
        self.assertEqual(self.run_apply(request), 2)
        self.assertEqual((self.app / "TextureQuest.exe").read_bytes(), self.old["TextureQuest.exe"])
        self.assertFalse((Path(request["request_path"]).parent / "helper-ready.json").exists())
        self.assert_preserved()

    def test_parent_stays_open_after_ready_timeout_no_mutation(self):
        request = self.stage()
        self.processes.wait_success = False
        self.assertEqual(self.run_apply(request), 2)
        self.assertEqual((self.app / "TextureQuest.exe").read_bytes(), self.old["TextureQuest.exe"])
        self.assertEqual(update._load(Path(request["request_path"]).parent / "result.json")["rollback"], "not_needed")
        self.assertFalse(self.processes.spawned)
        self.assert_preserved()

    def test_launch_requires_matching_ready_not_popen_return(self):
        request = self.stage()
        self.processes.on_spawn = lambda child, args: self.processes.living.pop(child["pid"])
        with self.assertRaisesRegex(update.AppUpdateError, "before it was ready"):
            update.launch_updater(request, processes=self.processes)
        self.assertEqual((self.app / "TextureQuest.exe").read_bytes(), self.old["TextureQuest.exe"])
        self.assert_preserved()

    def test_launch_ready_exact_identity_and_fixed_smoke_flag(self):
        request = self.stage()
        def ready(child, args):
            path = Path(request["request_path"])
            update._receipt(path.parent, "helper-ready.json", update._file(path), request["nonce"], helper=child)
        self.processes.on_spawn = ready
        result = update.launch_updater(request, smoke_test=True, processes=self.processes)
        self.assertEqual(result["status"], "helper_ready")
        self.assertEqual(self.processes.spawned[0][1][-1], "--smoke-test")
        self.assert_preserved()

    def test_request_tamper_and_nonce_cannot_authorize_update(self):
        request = self.stage()
        with self.assertRaisesRegex(update.AppUpdateError, "identity mismatch"):
            update._request(request["request_path"], "f" * 32)
        path = Path(request["request_path"])
        value = json.loads(path.read_bytes())
        value["data_dir"] = str(self.root / "elsewhere")
        path.write_bytes(raw(value))
        with self.assertRaisesRegex(update.AppUpdateError, "checksum"):
            update._request(path, request["nonce"])
        self.assert_preserved()

    def test_acknowledgement_rejects_wrong_child_or_version(self):
        request = self.stage()
        self.processes.as_helper(request)
        path = Path(request["request_path"])
        value, _, app, _, stage = update._request(path, request["nonce"])
        update._receipt(stage, "launched.json", update._file(path), request["nonce"], helper=self.processes.helper,
                        child=dict(pid=999, birth=999, exe=str(self.app / "TextureQuest.exe")))
        for name in update.MANAGED:
            source = stage / "install" / name
            target = self.app / name
            if target.is_dir():
                import shutil
                shutil.rmtree(target)
            else:
                target.unlink()
            source.rename(target)
        self.processes.current = dict(pid=888, birth=888, exe=str(self.app / "TextureQuest.exe"))
        with self.assertRaisesRegex(update.AppUpdateError, "different process"):
            update.acknowledge_startup(path, request["nonce"], app_dir=self.app, data_dir=self.app / "data", version="1.3.6", processes=self.processes)
        with self.assertRaisesRegex(update.AppUpdateError, "does not match"):
            update.acknowledge_startup(path, request["nonce"], app_dir=self.app, data_dir=self.app / "data", version="1.3.5", processes=self.processes)
        self.assert_preserved()

    def test_streaming_download_bounds_checksum_and_redirect_endpoint(self):
        data = self.archive.read_bytes()
        class Response(io.BytesIO):
            status = 200
            headers = {"Content-Length": str(len(data))}
            def geturl(self):
                return "https://release-assets.githubusercontent.com/owned.zip"
        update._download(self.installer, self.root / "download.zip", lambda *a, **k: Response(data), None)
        self.assertEqual((self.root / "download.zip").read_bytes(), data)
        with self.assertRaisesRegex(update.AppUpdateError, "exceeds"):
            update._download(self.installer, self.root / "overflow.zip", lambda *a, **k: Response(data + b"X"), None)
        with self.assertRaisesRegex(update.AppUpdateError, "checksum"):
            update._download(dict(self.installer, sha256="a" * 64), self.root / "bad-hash.zip", lambda *a, **k: Response(data), None)
        class Foreign(Response):
            def geturl(self):
                return "https://example.org/owned.zip"
        with self.assertRaisesRegex(update.AppUpdateError, "endpoint"):
            update._download(self.installer, self.root / "foreign.zip", lambda *a, **k: Foreign(data), None)

    def test_online_stage_refresh_mismatch_fails_before_download(self):
        with patch.object(update.versions, "_metadata", return_value={}), patch.object(update.versions, "_version_asset", return_value=None), \
                patch.object(update.versions, "_installer", return_value=dict(self.installer, sha256="f" * 64)), \
                patch.object(update, "_download") as download:
            with self.assertRaisesRegex(update.AppUpdateError, "changed"):
                update.stage_update(self.installer, app_dir=self.app, data_dir=self.app / "data", current_version="1.3.5")
            download.assert_not_called()

    def test_protected_archive_reader_denies_writes_and_changed_archive_refuses(self):
        with update._reader(self.archive) as retained:
            self.assertEqual(retained.read(2), b"PK")
            if sys.platform == "win32":
                with self.assertRaises(PermissionError):
                    with self.archive.open("wb"):
                        pass
        with self.assertRaisesRegex(update.AppUpdateError, "changed before protected extraction"):
            update._extract(self.archive, self.root / "protected-refusal", "1.3.6",
                            {"bytes": self.installer["bytes"], "sha256": "f" * 64})
        self.assert_preserved()

    def test_failed_new_root_move_restores_all_old_roots(self):
        request = self.stage()
        original, calls = update._move, []
        def failing(source, destination):
            calls.append((source, destination))
            if len(calls) == 8:
                raise OSError("Own late replacement fixture failure")
            return original(source, destination)
        with patch.object(update, "_move", side_effect=failing):
            self.assertEqual(self.run_apply(request), 2)
        value = update._request(request["request_path"], request["nonce"])[0]
        self.assertEqual(update._bundle(self.app, "1.3.5"), value["old_bundle"])
        self.assertEqual(update._load(Path(request["request_path"]).parent / "result.json")["rollback"], "restored")
        self.assert_preserved()

    def test_wrong_ready_identity_never_closes_or_changes_old_app(self):
        request = self.stage()
        def ready(child, args):
            path = Path(request["request_path"])
            update._receipt(path.parent, "helper-ready.json", update._file(path), request["nonce"], helper=dict(child, birth=child["birth"] + 1))
        self.processes.on_spawn = ready
        with self.assertRaisesRegex(update.AppUpdateError, "identity changed"):
            update.launch_updater(request, processes=self.processes)
        self.assert_preserved()

    def test_committed_update_does_not_become_failure_when_lock_cleanup_fails(self):
        request = self.stage()
        original = Path.unlink
        def unlink(path, *args, **kwargs):
            if path.name.startswith(".TextureQuest-update-lock-"):
                raise PermissionError("Own fixture cleanup failure")
            return original(path, *args, **kwargs)
        with patch.object(Path, "unlink", unlink):
            self.assertEqual(self.run_apply(request), 0)
        stage = Path(request["request_path"]).parent
        self.assertEqual(update._load(stage / "result.json")["status"], "committed")
        self.assertTrue((stage / "cleanup-warning.json").exists())
        self.assert_preserved()

    def test_native_spawn_hides_only_helper_and_shows_new_or_restored_gui(self):
        calls = []
        def popen(command, **kwargs):
            calls.append((command, kwargs))
            return type("OwnFakePopen", (), {"pid": 700})()
        identity = dict(pid=700, birth=123.5, exe=str(self.app / "TextureQuest.exe"))
        with patch.object(update.subprocess, "Popen", side_effect=popen), \
                patch.object(update, "process_identity", return_value=identity):
            for option in ("--apply-app-update", "--app-update-ack", "--app-update-failed"):
                self.assertEqual(update.WindowsProcesses().spawn(self.app / "TextureQuest.exe", [option, "own-fixture-request"], self.app), identity)
        self.assertEqual([kw["startupinfo"].wShowWindow for _, kw in calls], [0, 1, 1])
        self.assertTrue(all(kw["startupinfo"].dwFlags & update.subprocess.STARTF_USESHOWWINDOW for _, kw in calls))
        self.assertTrue(all(kw["creationflags"] & update.subprocess.CREATE_NO_WINDOW for _, kw in calls))
        self.assertTrue(all(kw["creationflags"] & update.subprocess.BELOW_NORMAL_PRIORITY_CLASS for _, kw in calls))
        self.assert_preserved()


if __name__ == "__main__":
    unittest.main()
