"""Owned loader adapter with invented D data; every Windows/process leaf is fake."""
from contextlib import ExitStack, nullcontext
import copy
import hashlib
import json
from pathlib import Path
import sys
import struct
import tempfile
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_sideload as module


def write_json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")


def file_pin(path):
    return dict(path=str(path), bytes=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest())


class BackgroundLoaderBackend(unittest.TestCase):
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / "build"
        base.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="loader-backend-", dir=base)
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.game = self.folder / "selected EQ2 folder"
        self.game.mkdir()
        (self.game / "paks").mkdir()
        (self.game / "EverQuest2.exe").write_bytes(b"invented, never executed")
        self.state_dir = self.folder / "portable data"
        self.state_dir.mkdir()
        self.prepared = dict(schema="texturequest-direct-prepared-v1", id="invented-download-identity", archives=[])
        write_json(self.state_dir / "direct-prepared.json", self.prepared)
        self.runtime_dir = self.folder / "runtime"
        self.runtime_dir.mkdir()
        files = {}
        for role, name in (("watcher", "TextureQuestWatch.exe"), ("dll", "TextureQuestBackground.dll"),
                           ("preparer", "prepare_full_pack.py"), ("catalog", "HANDOFF.json")):
            path = self.runtime_dir / name
            path.write_bytes(("invented never executed " + role).encode())
            files[role] = file_pin(path)
        self.descriptor = self.runtime_dir / "development-runtime.json"
        self.spec = dict(schema=module.SCHEMA, development=True, files=files)
        write_json(self.descriptor, self.spec)
        self.run_values, self.processes, self.calls = {}, {}, []
        self.managed = {"managed_count": 0, "archives": []}
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(module, "PREPARER_SHA256", files["preparer"]["sha256"]))
        self.stack.enter_context(patch.object(module, "DirectSwapper", return_value=MagicMock(status=MagicMock(side_effect=lambda game: self.managed))))
        self.original_prepare = module._prepare
        self.leaves = {}
        for name, function in (("_read_run", self.read_run), ("_write_run", self.write_run), ("_remove_run", self.remove_run),
                               ("_spawn_watcher", self.spawn), ("_signal_stop", self.stop),
                               ("_process_identity", lambda pid: self.processes.get(pid)), ("_prepare", self.prepare)):
            self.leaves[name] = self.stack.enter_context(patch.object(module, name, side_effect=function))
        self.manager = module.LoaderManager(self.state_dir, development_manifest=self.descriptor)

    def read_run(self, name):
        return self.run_values.get(name)

    def write_run(self, name, expected, command):
        self.assertEqual(self.run_values.get(name), expected)
        self.calls.append(("register", name, command))
        self.run_values[name] = command

    def remove_run(self, name, command):
        if self.run_values.get(name) not in (None, command):
            raise module.LoaderError("foreign startup preserved")
        self.calls.append(("unregister", name))
        self.run_values.pop(name, None)

    def prepare(self, runtime, game, prepared, output, progress):
        self.calls.append(("prepare", str(game), str(prepared), str(output)))
        output.mkdir(parents=True)
        binary = output / "READY.bin"
        binary.write_bytes(b"invented full-pack routing, never loaded")
        bindings = output.parent / "BINDINGS.bin"
        bindings.write_bytes(b"invented native freshness bindings, never loaded")
        ready = dict(schema="tq-fullpack-ready-v1", game=str(game), output=str(output),
                     manifest=file_pin(binary), prepared=file_pin(prepared), source_vpl=dict(qualification="invented"), applied=310)
        ready_path = output / "READY.json"
        write_json(ready_path, ready)
        return dict(ready_json=file_pin(ready_path), manifest=file_pin(binary), bindings=file_pin(bindings), output=str(output),
                    prepared=ready["prepared"], source_vpl=ready["source_vpl"], applied=310)

    def spawn(self, arguments):
        self.calls.append(("spawn", list(arguments)))
        self.watcher = dict(pid=1001, birth100ns=123456789, exe=arguments[0])
        self.processes[1001] = dict(self.watcher)
        folder = Path(arguments[arguments.index("--status-dir") + 1])
        self.status_path = folder / "status.json"
        owner = (folder / "OWNER.bin").read_bytes()
        write_json(self.status_path, dict(schema="tq-background-status-v2", watcher_pid=1001,
            watcher_birth100ns=123456789, install_nonce=arguments[-1].split("-")[-1],
            owner_sha256=hashlib.sha256(owner).hexdigest(), run_id="a" * 32,
            state="armed", message="Armed for future EQ2 sessions; open clients stay unchanged.", sessions=[]))
        return dict(self.watcher)

    def stop(self, event):
        self.calls.append(("stop", event))
        self.processes.pop(1001, None)
        value = module._json(self.status_path)
        value.update(state="stopped", message="Watcher stopped; DLL sessions remain resident.", sessions=[])
        write_json(self.status_path, value)

    def install(self):
        return self.manager.install(str(self.game), copy.deepcopy(self.prepared))

    def add_session(self, *, pid=2002, birth=987654321):
        self.processes[pid] = dict(pid=pid, birth100ns=birth, exe=str(self.game / "EverQuest2.exe"))
        value = module._json(self.status_path)
        value.update(state="attached", sessions=[dict(pid=pid, birth100ns=birth, state="attached", redirects=10,
            private_index_active=True, observation_only=False)])
        write_json(self.status_path, value)

    def test_missing_runtime_is_unavailable_without_any_os_or_prepare_calls(self):
        manager = module.LoaderManager(self.state_dir, development_manifest=self.folder / "missing.json")
        value = manager.status(str(self.game))
        self.assertFalse(value["available"])
        self.assertFalse(value["installed"])
        self.assertIn("not included", value["message"])
        self.assertFalse(manager.cache_in_use())
        with self.assertRaisesRegex(module.LoaderError, "not included"):
            manager.install(str(self.game), self.prepared)
        self.assertEqual(self.calls, [])

    def test_install_prepares_fullpack_and_registers_one_exact_direct_launch_agnostic_watcher(self):
        # An already open game is neither stopped nor injected by this adapter.
        self.processes[3003] = dict(pid=3003, birth100ns=99, exe=str(self.game / "EverQuest2.exe"))
        value = self.install()
        self.assertTrue(value["installed"] and value["running"] and value["available"])
        self.assertEqual(value["active_sessions"], 0)
        self.assertIn("open clients stay unchanged", value["message"])
        self.assertEqual([call[0] for call in self.calls], ["prepare", "register", "spawn"])
        arguments = self.calls[-1][1]
        self.assertEqual(arguments[1::2], ["--game-dir", "--manifest", "--manifest-sha256", "--bindings", "--bindings-sha256", "--status-dir", "--stop-event"])
        self.assertEqual(arguments[2], str(self.game))
        self.assertNotIn("shortcut", " ".join(arguments).lower())
        self.assertNotIn("--launch", arguments)
        record = self.manager._record(self.game)
        self.assertEqual(record["phase"], "installed")
        self.assertEqual(self.run_values[record["run_name"]], module.subprocess.list2cmdline(arguments))
        self.assertEqual(record["arguments"], arguments)
        self.assertTrue(self.manager.cache_in_use())
        self.assertEqual(self.processes[3003]["birth100ns"], 99)
        self.leaves["_prepare"].assert_called_once()
        with self.assertRaisesRegex(module.LoaderError, "Uninstall"):
            self.install()
        self.leaves["_prepare"].assert_called_once()

    def test_fresh_managed_pak_and_changed_prepared_identity_refuse_before_preparation(self):
        self.managed = {"managed_count": 1}
        with self.assertRaisesRegex(module.LoaderError, "official EQ2 patcher"):
            self.install()
        self.managed = {"archives": [{"managed": True}]}
        with self.assertRaisesRegex(module.LoaderError, "official EQ2 patcher"):
            self.install()
        self.managed = {}
        with self.assertRaisesRegex(module.LoaderError, "direct-prepared"):
            self.manager.install(str(self.game), dict(self.prepared, id="changed"))
        self.leaves["_prepare"].assert_not_called()
        self.assertEqual(self.calls, [])

    def test_partial_preparation_is_preserved_and_never_automatically_retried(self):
        def fail(runtime, game, prepared, output, progress):
            output.mkdir(parents=True)
            (output / "partial-preserve.bin").write_bytes(b"retained failed preparation")
            raise OSError("invented preparer refusal")
        self.leaves["_prepare"].side_effect = fail
        with self.assertRaisesRegex(OSError, "preparer refusal"):
            self.install()
        record = self.manager._record(self.game)
        self.assertEqual(record["phase"], "failed")
        output = Path(record["epoch_root"]) / "pack"
        self.assertEqual((output / "partial-preserve.bin").read_bytes(), b"retained failed preparation")
        self.assertFalse(self.manager.status(str(self.game))["installed"])
        self.assertEqual(self.leaves["_prepare"].call_count, 1)
        self.assertFalse(self.manager.cache_in_use())
        self.leaves["_spawn_watcher"].assert_not_called()
        self.leaves["_write_run"].assert_not_called()

    def test_uninstall_uses_only_owned_run_and_stop_event_and_retains_live_session_after_watcher_exit(self):
        self.install()
        self.add_session()
        record = self.manager._record(self.game)
        result = self.manager.uninstall(str(self.game))
        self.assertFalse(result["installed"] or result["running"])
        self.assertEqual(result["active_sessions"], 1)
        self.assertIn("until they exit", result["message"])
        self.leaves["_signal_stop"].assert_called_once_with(record["stop_event"])
        self.assertIn(2002, self.processes)
        self.assertTrue(self.manager.cache_in_use())
        self.assertTrue(Path(record["ready"]["manifest"]["path"]).exists())
        self.assertEqual(self.run_values, {})
        self.manager.uninstall(str(self.game))  # Exact completed request is idempotent.
        self.leaves["_signal_stop"].assert_called_once()
        self.processes.pop(2002)
        self.assertEqual(self.manager.status(str(self.game))["active_sessions"], 0)
        self.assertFalse(self.manager.cache_in_use())

    def test_pid_reuse_is_not_a_live_owned_session(self):
        self.install()
        self.add_session()
        self.processes[2002]["birth100ns"] += 1
        self.assertEqual(self.manager.status(str(self.game))["active_sessions"], 0)

    def test_missing_runtime_files_still_allows_owned_uninstall(self):
        self.install()
        Path(self.spec["files"]["dll"]["path"]).unlink()
        value = self.manager.status(str(self.game))
        self.assertTrue(value["installed"] and value["available"])
        self.assertIn("Uninstall remains available", value["message"])
        result = self.manager.uninstall(str(self.game))
        self.assertFalse(result["installed"])
        self.leaves["_remove_run"].assert_called_once()

    def test_foreign_run_value_wrong_game_and_tampered_receipt_are_preserved(self):
        self.install()
        record = self.manager._record(self.game)
        foreign = '"C:\\other\\unrelated.exe" --leave-it'
        self.run_values[record["run_name"]] = foreign
        before = self.manager._path(self.game).read_bytes()
        with self.assertRaisesRegex(module.LoaderError, "ownership"):
            self.manager.uninstall(str(self.game))
        self.assertEqual(self.run_values[record["run_name"]], foreign)
        self.assertEqual(self.manager._path(self.game).read_bytes(), before)
        self.assertTrue(self.manager.cache_in_use())
        self.leaves["_remove_run"].assert_not_called()
        other = self.folder / "another-game"
        other.mkdir()
        with self.assertRaisesRegex(module.LoaderError, "No owned loader"):
            self.manager.uninstall(str(other))
        path = self.manager._path(self.game)
        tampered = module._json(path)
        tampered["game"] = str(other)
        write_json(path, tampered)
        with self.assertRaisesRegex(module.LoaderError, "receipt changed"):
            self.manager.status(str(self.game))
        self.assertTrue(self.manager.cache_in_use())

    def test_registration_cas_refuses_concurrent_change_without_overwrite(self):
        self.install()
        record = self.manager._record(self.game)
        path = self.manager._path(self.game)
        changed = module._sealed(dict(record, error="new concurrent writer"))
        write_json(path, changed)
        before = path.read_bytes()
        with self.assertRaisesRegex(module.LoaderError, "concurrently"):
            self.manager._advance(record, "uninstalled")
        self.assertEqual(path.read_bytes(), before)

    def test_run_registration_failure_leaves_explicit_partial_receipt_and_no_spawn(self):
        self.leaves["_write_run"].side_effect = OSError("registry write failed")
        with self.assertRaisesRegex(OSError, "registry write failed"):
            self.install()
        self.assertEqual(self.manager._record(self.game)["phase"], "failed")
        self.leaves["_spawn_watcher"].assert_not_called()
        self.assertEqual(self.run_values, {})

    def test_after_registration_spawn_failure_remains_uninstallable(self):
        self.leaves["_spawn_watcher"].side_effect = module.WatcherStartError("watcher start failed")
        with self.assertRaisesRegex(module.WatcherStartError, "watcher start failed"):
            self.install()
        value = self.manager.status(str(self.game))
        self.assertTrue(value["installed"] and value["available"])
        result = self.manager.uninstall(str(self.game))
        self.assertFalse(result["installed"])
        self.leaves["_signal_stop"].assert_not_called()

    def test_unknown_spawn_outcome_preserves_registration_and_blocks_false_absence_or_retry(self):
        self.leaves["_spawn_watcher"].side_effect = module.WatcherStartError("query failed after process start", started_pid=4444)
        with self.assertRaisesRegex(module.WatcherStartError, "query failed"):
            self.install()
        self.assertTrue(self.manager.cache_in_use())
        self.assertTrue(self.run_values)
        with self.assertRaisesRegex(module.LoaderError, "startup outcome is unknown"):
            self.manager.status(str(self.game))
        with self.assertRaisesRegex(module.LoaderError, "startup outcome is unknown"):
            self.manager.uninstall(str(self.game))
        self.assertEqual(self.leaves["_spawn_watcher"].call_count, 1)
        self.leaves["_remove_run"].assert_not_called()

    def test_unknown_existing_installation_directory_is_never_claimed_or_deleted(self):
        fixed = "a" * 32
        home = self.manager.base / "installations" / fixed
        home.mkdir(parents=True)
        (home / "unknown.txt").write_bytes(b"preserve me")
        with patch.object(module.uuid, "uuid4", return_value=type("Fixed", (), {"hex": fixed})()), \
             self.assertRaisesRegex(module.LoaderError, "Unknown loader installation directory"):
            self.install()
        self.assertEqual((home / "unknown.txt").read_bytes(), b"preserve me")
        self.leaves["_prepare"].assert_not_called()

    def test_runtime_mutation_and_development_scope_are_unavailable(self):
        self.assertTrue(self.manager.status(str(self.game))["available"])
        Path(self.spec["files"]["watcher"]["path"]).write_bytes(b"changed executable bytes")
        self.assertFalse(self.manager.status(str(self.game))["available"])
        with patch.object(module.sys, "frozen", True, create=True), self.assertRaisesRegex(module.LoaderError, "Development loader"):
            module.LoaderManager(self.state_dir, development_manifest=self.descriptor)

    def test_nested_reviewed_v2_preparer_uses_native_bundle_epoch_root(self):
        nested = self.runtime_dir / "preparer-v2"
        nested.mkdir()
        nested_source = nested / "prepare_full_pack.py"
        nested_source.write_bytes(Path(self.spec["files"]["preparer"]["path"]).read_bytes())
        self.spec["files"]["preparer"] = file_pin(nested_source)
        write_json(self.descriptor, self.spec)
        runtime = self.manager._runtime()
        self.assertEqual(runtime["preparation_root"], str(self.runtime_dir / "background-epochs"))
        self.assertNotIn("preparer-v2", runtime["preparation_root"])

    def test_malformed_status_or_access_denial_blocks_cache_without_claiming_absence(self):
        self.install()
        malformed = module._json(self.status_path)
        malformed["watcher_birth100ns"] = True
        write_json(self.status_path, malformed)
        with self.assertRaisesRegex(module.LoaderError, "watcher status"):
            self.manager.status(str(self.game))
        self.assertTrue(self.manager.cache_in_use())
        self.spawn(self.manager._record(self.game)["arguments"])
        self.leaves["_process_identity"].side_effect = PermissionError("query denied")
        with self.assertRaisesRegex(PermissionError, "query denied"):
            self.manager.status(str(self.game))
        self.assertTrue(self.manager.cache_in_use())

    def test_native_bindings_encode_exact_distinct_route_union_and_freshness_without_extra_paths(self):
        original = self.game / "stock-source.vpk"
        replacement = self.runtime_dir / "replacement-🧙.vpk"
        ignored = self.runtime_dir / "catalog-extra.json"
        for index, path in enumerate((original, replacement, ignored)):
            path.write_bytes(b"invented" * (index + 1))
        pins = [dict(file_pin(path), stat5=list(module._fingerprint(path))) for path in (original, replacement, ignored)]
        ready = dict(schema="tq-fullpack-ready-v1", routes=[dict(original=str(original), replacement=str(replacement))],
                     route_count=1, bound_files=pins)
        target = self.folder / "BINDINGS.bin"
        pin = module._make_bindings(ready, target)
        blob = target.read_bytes()
        self.assertEqual(blob[:8], b"TQCFWB01")
        self.assertEqual(struct.unpack_from("<II", blob, 8), (1, 2))
        rows, offset = {}, 16
        for _ in range(2):
            units, reserved, size, stamp = struct.unpack_from("<IIQq", blob, offset)
            offset += 24
            path = blob[offset:offset + units * 2].decode("utf-16-le")
            offset += units * 2
            self.assertEqual(reserved, 0)
            rows[path] = (size, stamp)
        self.assertEqual(offset, len(blob))
        self.assertEqual(set(rows), {str(original), str(replacement)})
        for row in pins[:2]:
            self.assertEqual(rows[row["path"]], (row["bytes"], row["stat5"][3]))
        self.assertEqual(pin["sha256"], hashlib.sha256(blob).hexdigest())
        original.write_bytes(b"changed after full verification")
        with self.assertRaisesRegex(module.LoaderError, "changed before native bindings"):
            module._make_bindings(ready, self.folder / "changed-bindings.bin")
        self.assertFalse((self.folder / "changed-bindings.bin").exists())

    def test_saved_ready_reuse_requires_full_exact_verification_and_does_not_reprepare(self):
        old = self.runtime_dir / "already-prepared"
        old.mkdir()
        original = self.game / "assetslib.vpl"
        original.write_bytes(b"invented stock index")
        private = old / "assetslib.vpl"
        private.write_bytes(b"invented private index")
        binary = old / "READY.bin"
        binary.write_bytes(b"invented manifest")
        prepared_path = self.state_dir / "direct-prepared.json"
        pins = [dict(file_pin(path), stat5=list(module._fingerprint(path))) for path in (original, private)]
        ready = dict(schema="tq-fullpack-ready-v1", game=str(self.game), output=str(old), prepared=file_pin(prepared_path),
                     manifest=file_pin(binary), source_vpl=pins[0], applied=1,
                     routes=[dict(original=str(original), replacement=str(private))], route_count=1, bound_files=pins)
        ready_path = old / "READY.json"
        write_json(ready_path, ready)
        runtime = self.manager._runtime()
        runtime = dict(runtime, prepared_ready=file_pin(ready_path))
        helper = MagicMock()
        helper.verify_ready.return_value = dict(status="VERIFIED_READY", game=str(self.game), manifest=ready["manifest"])
        fresh = self.runtime_dir / "fresh-owned-install"
        fresh.mkdir()
        actual_prepare = self.leaves["_prepare"]
        actual_prepare.side_effect = None
        # Call the original Python adapter function; only the reviewed-helper leaf is fake.
        with patch.object(module, "_preparer", return_value=nullcontext(helper)):
            result = self.original_prepare(runtime, self.game, prepared_path, fresh / "pack", None)
        helper.prepare.assert_not_called()
        helper.verify_ready.assert_called_once_with(ready_path)
        self.assertEqual(result["output"], str(old))
        self.assertTrue((fresh / "BINDINGS.bin").is_file())
        self.assertFalse((fresh / "pack").exists())
        self.assertEqual(private.read_bytes(), b"invented private index")
        changed = dict(ready, game=str(self.folder / "different-game"))
        write_json(ready_path, changed)
        runtime["prepared_ready"] = file_pin(ready_path)
        with patch.object(module, "_preparer", return_value=nullcontext(helper)), self.assertRaisesRegex(module.LoaderError, "another game"):
            self.original_prepare(runtime, self.game, prepared_path, fresh / "pack", None)


if __name__ == "__main__":
    unittest.main()
