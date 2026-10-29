"""V2 ownership, atomic status reads and conflict serialization; tiny D fixtures.

Registry, process queries, spawning and full-pack preparation are mocked. Only
the explicit live-reader boundary uses real Windows FILE handles and D renames.
"""
from contextlib import contextmanager
import builtins
import copy
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_sideload as module
import texturequest_direct as direct
import texturequest_fast_install as fast
from texture_swapper import core
import test_background_loader_backend as fixtures


class ProtocolV2(unittest.TestCase):
    setUp = fixtures.BackgroundLoaderBackend.setUp
    read_run = fixtures.BackgroundLoaderBackend.read_run
    write_run = fixtures.BackgroundLoaderBackend.write_run
    remove_run = fixtures.BackgroundLoaderBackend.remove_run
    prepare = fixtures.BackgroundLoaderBackend.prepare
    stop = fixtures.BackgroundLoaderBackend.stop
    install = fixtures.BackgroundLoaderBackend.install

    def spawn(self, arguments):
        self.calls.append(("spawn", list(arguments)))
        self.watcher = dict(pid=1001, birth100ns=123456789, exe=arguments[0])
        self.processes[1001] = dict(self.watcher)
        folder = Path(arguments[arguments.index("--status-dir") + 1])
        self.status_path = folder / "status.json"
        owner = (folder / "OWNER.bin").read_bytes()
        nonce = arguments[-1].split("-")[-1]
        fixtures.write_json(self.status_path, dict(schema="tq-background-status-v2", install_nonce=nonce,
            owner_sha256=hashlib.sha256(owner).hexdigest(), run_id="a" * 32,
            watcher_pid=1001, watcher_birth100ns=123456789, state="attaching",
            message="Installed; checking a future session. Existing games remain unchanged.", sessions=[]))
        return dict(self.watcher)

    def put_session(self, *, pid=2002, birth=987654321, state="attached", observation_only=False):
        if birth:
            self.processes[pid] = dict(pid=pid, birth100ns=birth, exe=str(self.game / "EverQuest2.exe"))
        value = module._json(self.status_path)
        value.update(state="attached", sessions=[dict(pid=pid, birth100ns=birth, state=state,
            redirects=0 if observation_only else 10, private_index_active=not observation_only,
            observation_only=observation_only)])
        fixtures.write_json(self.status_path, value)

    def previous_run(self, value, new_run="b" * 32):
        folder = self.status_path.parent / "history" / ("run-" + new_run)
        folder.mkdir(parents=True)
        previous = folder / "previous-status.json"
        fixtures.write_json(previous, value)
        return previous

    def test_owner_is_exact_and_durable_before_registration_and_attaching_is_accepted(self):
        original_register = self.write_run
        def registered(name, expected, command):
            record = self.manager._record(self.game)
            raw = Path(record["owner"]["path"]).read_bytes()
            self.assertEqual(raw[:8], b"TQBGOWN2")
            version, units = struct.unpack_from("<II", raw, 8)
            self.assertEqual(version, 2)
            self.assertEqual(raw[16:48].decode("ascii"), record["installation_id"])
            self.assertEqual(raw[48:112].decode("ascii"), record["ready"]["manifest"]["sha256"])
            self.assertEqual(raw[112:176].decode("ascii"), record["ready"]["bindings"]["sha256"])
            self.assertEqual(len(raw), 176 + 2 * units)
            expected_game = "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in str(self.game))
            self.assertEqual(raw[176:].decode("utf-16-le"), expected_game)
            self.assertEqual(record["phase"], "prepared")
            original_register(name, expected, command)
        self.leaves["_write_run"].side_effect = registered
        result = self.install()
        self.assertTrue(result["installed"] and result["running"])
        self.assertEqual(self.manager._record(self.game)["phase"], "installed")
        # Unicode ASCII-only lowering matches the native wire, not casefold.
        unicode_path = self.folder / "EQ2-Ä-🧙"
        encoded = module._owner_bytes(unicode_path, "c" * 32, "d" * 64, "e" * 64)
        self.assertIn("Ä-🧙", encoded[176:].decode("utf-16-le"))

    def test_unknown_identity_is_diagnostic_only_under_explicit_never_attached_shape(self):
        self.install()
        self.put_session(birth=0, state="restart_needed", observation_only=True)
        self.assertEqual(self.manager.status(self.game)["active_sessions"], 0)
        valid = module._json(self.status_path)
        for changed in ({"observation_only": False}, {"redirects": 1},
                        {"private_index_active": True}, {"state": "attaching"}):
            bad = copy.deepcopy(valid)
            bad["sessions"][0].update(changed)
            fixtures.write_json(self.status_path, bad)
            with self.assertRaisesRegex(module.LoaderError, "never-attached"):
                self.manager.status(self.game)
            self.assertTrue(self.manager.cache_in_use())
        fixtures.write_json(self.status_path, valid)
        self.assertFalse(self.manager.uninstall(self.game)["installed"])
        self.assertFalse(self.manager.cache_in_use())

    def test_previous_run_retains_live_session_after_new_watcher_baseline_and_uninstall(self):
        self.install()
        self.put_session()
        old = module._json(self.status_path)
        previous = self.previous_run(old)
        before = previous.read_bytes()
        current = dict(old, run_id="b" * 32, state="armed", sessions=[])
        fixtures.write_json(self.status_path, current)
        result = self.manager.status(self.game)
        self.assertEqual(result["active_sessions"], 1)
        # Repeated historical/current representations count one PID/birth.
        current["sessions"] = old["sessions"]
        fixtures.write_json(self.status_path, current)
        self.assertEqual(self.manager.status(self.game)["active_sessions"], 1)
        result = self.manager.uninstall(self.game)
        self.assertFalse(result["installed"])
        self.assertEqual(result["active_sessions"], 1)
        self.assertTrue(self.manager.cache_in_use())
        self.assertEqual(previous.read_bytes(), before)
        self.processes[2002]["birth100ns"] += 1  # PID reused after old game exit.
        self.assertEqual(self.manager.status(self.game)["active_sessions"], 0)
        self.assertFalse(self.manager.cache_in_use())

    def test_prior_run_wrong_owner_or_nonce_is_preserved_and_refuses_ownership_release(self):
        self.install()
        old = module._json(self.status_path)
        previous = self.previous_run(old)
        for changed in ({"install_nonce": "f" * 32}, {"owner_sha256": "f" * 64}, {"run_id": "bad"}):
            fixtures.write_json(previous, dict(old, **changed))
            before = previous.read_bytes()
            with self.assertRaisesRegex(module.LoaderError, "unowned"):
                self.manager.status(self.game)
            self.assertTrue(self.manager.cache_in_use())
            self.assertEqual(previous.read_bytes(), before)
        self.leaves["_remove_run"].assert_not_called()

    def test_changed_owner_cannot_be_hidden_by_valid_looking_status(self):
        self.install()
        record = self.manager._record(self.game)
        Path(record["owner"]["path"]).write_bytes(b"unknown owner")
        with self.assertRaisesRegex(module.LoaderError, "ownership marker changed"):
            self.manager.uninstall(self.game)
        self.leaves["_remove_run"].assert_not_called()
        self.leaves["_signal_stop"].assert_not_called()
        self.assertTrue(self.manager.cache_in_use())

    def test_installed_runtime_missing_still_allows_owned_uninstall(self):
        self.install()
        Path(self.spec["files"]["dll"]["path"]).unlink()
        self.assertTrue(self.manager.status(self.game)["available"])
        result = self.manager.uninstall(self.game)
        self.assertFalse(result["installed"])
        self.assertFalse(self.run_values)

    def test_external_prepared_source_is_development_only_readonly_and_exact(self):
        external = self.folder / "readonly saved downloads"
        external.mkdir()
        source = external / "direct-prepared.json"
        fixtures.write_json(source, self.prepared)
        before = source.read_bytes()
        self.spec["prepared_source"] = fixtures.file_pin(source)
        fixtures.write_json(self.descriptor, self.spec)
        self.install()
        self.assertEqual(self.calls[0][2], str(source))
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(list(external.iterdir()), [source])
        self.manager.uninstall(self.game)
        with self.assertRaisesRegex(module.LoaderError, "no longer matches"):
            self.manager.install(self.game, dict(self.prepared, id="different"))
        self.assertEqual(self.leaves["_spawn_watcher"].call_count, 1)
        # Even a relative external source is rejected by the public descriptor.
        public = copy.deepcopy(self.spec)
        public["development"] = False
        public["files"] = {name: dict(pin, path=Path(pin["path"]).name) for name, pin in public["files"].items()}
        fixtures.write_json(self.descriptor, public)
        public_manager = module.LoaderManager(self.state_dir)
        public_manager.descriptor = self.descriptor
        self.assertIsNone(public_manager._runtime())
        self.assertIn("development descriptor", public_manager.runtime_error)

    def test_bindings_refuse_native_unrepresentable_bounds_before_publication(self):
        file_path = self.folder / "tiny-original.vpk"
        file_path.write_bytes(b"tiny")
        actual = module._fingerprint(file_path)
        for number, (size, stamp) in enumerate(((-1, 100), (2**63, 100), (4, -100), (4, 101), (4, 2**63))):
            fake = (actual[0], actual[1], size, stamp, actual[4])
            pin = dict(path=str(file_path), bytes=size, stat5=list(fake))
            ready = dict(schema="tq-fullpack-ready-v1", routes=[dict(original=str(file_path), replacement=str(file_path))],
                         route_count=1, bound_files=[pin])
            target = self.folder / ("invalid-bindings-%s.bin" % number)
            with patch.object(module, "_fingerprint", return_value=fake), self.assertRaisesRegex(module.LoaderError, "before native bindings"):
                module._make_bindings(ready, target)
            self.assertFalse(target.exists())


class LiveStatusSnapshot(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1] / "build"
        root.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="live-status-v2-", dir=root)
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)

    def test_open_uses_read_and_delete_sharing_not_write_sharing(self):
        api = MagicMock()
        api.CreateFileW.return_value = 123
        path = self.folder / "status.json"
        with patch.object(module, "_live_api", return_value=api):
            self.assertEqual(module._open_live(path), 123)
        self.assertEqual(api.CreateFileW.call_args.args[1:3], (0x80000000, 0x1 | 0x4))

    def test_changed_held_file_or_oversize_refuses_and_always_closes(self):
        for stamps in (((1, 2, 3, 4), (1, 2, 4, 4)), ((1, 2, module.MAX_JSON + 1, 4),)):
            with patch.object(module, "_open_live", return_value=123), patch.object(module, "_live_window", side_effect=stamps), \
                 patch.object(module, "_read_live", side_effect=[b"old", b""]), patch.object(module, "_close_live") as close:
                with self.assertRaises(module.LoaderError):
                    module._live_status_bytes(self.folder / "status.json")
                close.assert_called_once_with(123)

    @unittest.skipUnless(os.name == "nt", "Windows atomic publication boundary")
    def test_atomic_writer_can_replace_while_real_reader_keeps_old_complete_version(self):
        path = self.folder / "status.json"
        old, new = b'{"version":"old","sessions":[]}', b'{"version":"new","sessions":[1]}'
        path.write_bytes(old)
        opened, replaced = threading.Event(), threading.Event()
        original_read = module._read_live
        outcome = {}
        def paused_read(handle, count):
            if not opened.is_set():
                opened.set()
                if not replaced.wait(5):
                    raise AssertionError("writer did not complete while retained reader was open")
            return original_read(handle, count)
        def reader():
            try:
                outcome["bytes"] = module._live_status_bytes(path)
            except BaseException as error:
                outcome["error"] = error
        with patch.object(module, "_read_live", side_effect=paused_read):
            thread = threading.Thread(target=reader)
            thread.start()
            try:
                self.assertTrue(opened.wait(5))
                pending = self.folder / "status.pending"
                pending.write_bytes(new)
                # MoveFileExW(REPLACE_EXISTING) fails WinError 5 for a held
                # destination on this host, even with full share7. Native V2
                # uses ReplaceFileW for existing latest-status publication.
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.ReplaceFileW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPCWSTR,
                    wintypes.DWORD, ctypes.c_void_p, ctypes.c_void_p]
                kernel.ReplaceFileW.restype = wintypes.BOOL
                if not kernel.ReplaceFileW(str(path), str(pending), None, 0, None, None):
                    raise ctypes.WinError(ctypes.get_last_error())
            finally:
                replaced.set()
                thread.join(5)
            self.assertFalse(thread.is_alive())
        if "error" in outcome:
            raise outcome["error"]
        self.assertEqual(outcome["bytes"], old)
        self.assertEqual(module._live_status_bytes(path), new)
        self.assertEqual(module._json(path), json.loads(new))


class PhysicalInstallSerialization(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1] / "build"
        root.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="pak-loader-lock-v2-", dir=root)
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.swapper = direct.DirectSwapper(self.folder / "data")
        self.game = self.folder / "fake-game"
        (self.game / "paks").mkdir(parents=True)
        (self.game / "EverQuest2.exe").write_bytes(b"never executed")

    def test_background_transition_after_ui_precheck_refuses_inside_both_real_locks(self):
        state = {"owned": False}
        at_operation, changed = threading.Event(), threading.Event()
        original_lock = core._lock
        failures = []
        @contextmanager
        def gate(path):
            if Path(path).name == "operation.lock" and threading.current_thread().name == "stale-pak-worker":
                at_operation.set()
                if not changed.wait(5):
                    raise AssertionError("loader transition never completed")
            with original_lock(path):
                yield
        def queried():
            # A new manager is created at the mutation boundary, under both locks.
            for path in (self.swapper.state_dir / "operation.lock", self.swapper.state_dir / "sideload/helper-owner.lock"):
                with self.assertRaises(RuntimeError):
                    with original_lock(path):
                        pass
            return state["owned"]
        def physical():
            try:
                self.swapper.install(dict(schema=direct.SCHEMA), self.game)
            except BaseException as error:
                failures.append(error)
        self.assertFalse(state["owned"])  # The UI's earlier view.
        with patch.object(core, "_lock", side_effect=gate), patch.object(module, "LoaderManager") as manager, \
             patch.object(self.swapper, "preflight") as preflight, patch.object(fast, "copy_archive") as copied:
            manager.return_value.cache_in_use.side_effect = queried
            thread = threading.Thread(target=physical, name="stale-pak-worker")
            thread.start()
            try:
                self.assertTrue(at_operation.wait(5))
                with original_lock(self.swapper.state_dir / "operation.lock"), original_lock(self.swapper.state_dir / "sideload/helper-owner.lock"):
                    state["owned"] = True
            finally:
                changed.set()
                thread.join(5)
            self.assertFalse(thread.is_alive())
            preflight.assert_not_called()
            copied.assert_not_called()
        self.assertEqual(len(failures), 1)
        self.assertIsInstance(failures[0], RuntimeError)
        self.assertIn("Uninstall the background loader", str(failures[0]))
        self.assertEqual(list((self.game / "paks").iterdir()), [])

    def test_legacy_install_path_refuses_before_core_install_when_owner_is_unknown(self):
        with patch.object(module, "LoaderManager") as manager, patch.object(core.Swapper, "install") as original:
            manager.return_value.cache_in_use.return_value = True
            with self.assertRaisesRegex(RuntimeError, "background loader"):
                self.swapper.install(dict(schema="texture-swapper-prepared-v1"), self.game)
            original.assert_not_called()

    def test_entirely_absent_optional_module_allows_pak_but_present_backend_errors_refuse(self):
        original_import = builtins.__import__
        def absent(name, *args, **kwargs):
            if name == "texturequest_sideload":
                raise ModuleNotFoundError("optional loader omitted", name=name)
            return original_import(name, *args, **kwargs)
        with patch.object(builtins, "__import__", side_effect=absent):
            self.swapper._assert_background_inactive()
        with patch.object(module, "LoaderManager", side_effect=PermissionError("present backend unreadable")), \
             self.assertRaisesRegex(PermissionError, "present backend"):
            self.swapper._assert_background_inactive()


if __name__ == "__main__":
    unittest.main()
