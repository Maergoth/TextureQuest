"""Portable app contract: tiny D FILEs, invented native leaves, no GUI/build/process actions."""
from contextlib import ExitStack, redirect_stdout
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT))
import texturequest_sideload as loader
import texturequest_app as app
import build_installer as builder


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


class PortableContract(unittest.TestCase):
    def setUp(self):
        cases = Path(os.environ.get("TEXTUREQUEST_PORTABLE_CASE_ROOT", str(ROOT / "build")))
        cases.mkdir(parents=True, exist_ok=True)
        self.folder = Path(tempfile.mkdtemp(prefix="portable-loader-contract-", dir=cases))
        self.bundle = self.folder / "bundled resources"
        self.payload = self.bundle / "sideload"
        names = [*loader.PORTABLE_ROLES.values(), "LICENSE-Detours.md", "catalog/current-archives.json",
                 *("catalog/members-%02d.json" % n for n in range(7))]
        rows = []
        for name in sorted(names):
            path = self.payload / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("invented never executed " + name).encode("ascii"))
            rows.append(dict(path=name, size=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        self.spec = dict(schemaVersion=2, development=False, roles=dict(loader.PORTABLE_ROLES), files=rows,
                         payload_id=hashlib.sha256(json.dumps(rows, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()).hexdigest())
        self.descriptor = self.bundle / "texturequest-sideload-runtime.json"
        write_json(self.descriptor, self.spec)
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        helper = next(row for row in rows if row["path"] == loader.PORTABLE_ROLES["helper"])
        self.stack.enter_context(patch.object(loader, "PORTABLE_PREPARER_SHA256", helper["sha256"]))
        self.state = self.folder / "portable data"
        self.state.mkdir()
        self.game = self.folder / "selected game"
        self.game.mkdir()
        (self.game / "paks").mkdir()
        (self.game / "EverQuest2.exe").write_bytes(b"invented never executed")
        self.manager = loader.LoaderManager(self.state)
        self.manager.descriptor = self.descriptor

    def legacy_record(self, game=None, state=None):
        manager = loader.LoaderManager(state or self.state)
        game = game or self.game
        runtime = dict(files={role: dict(path=str(self.folder / (role + ".old")), bytes=7, sha256="1" * 64)
                              for role in ("watcher", "dll", "preparer", "catalog")},
                       preparation_root=str(self.folder / "retained old D runtime" / "background-epochs"))
        identity = hashlib.sha256(str(game).encode()).hexdigest()[:32]
        epoch = Path(runtime["preparation_root"]) / identity
        record = dict(schema="tq-background-installation-v1", installation_id=identity, revision=4, phase="installed",
                      game=str(game), game_key=manager._key(game), runtime=runtime, epoch_root=str(epoch),
                      status_dir=str(epoch / "watcher"), stop_event="Local\\TQWatch-" + identity,
                      run_name="TextureQuestLoader-" + manager._key(game))
        write_json(manager._path(game), loader._sealed(record))
        return manager, record

    def test_open_status_and_bundle_admission_copy_nothing_and_notice_added_files(self):
        with patch.object(loader, "_read_run", side_effect=AssertionError("no existing record")), \
             patch.object(loader, "_spawn_watcher", side_effect=AssertionError("never launch")):
            self.assertTrue(self.manager.status(str(self.game))["available"])
            self.assertFalse(self.manager.base.exists())
            foreign = self.payload / "unexpected-file.bin"
            foreign.write_bytes(b"unknown retained")
            self.assertFalse(self.manager.status(str(self.game))["available"])
            self.assertTrue(foreign.exists())
            self.assertFalse(self.manager.base.exists())

    def test_frozen_author_payload_matches_final_pin_and_normal_descriptor_contract(self):
        author = ROOT / "sideload-runtime"
        with patch.object(loader, "PORTABLE_PREPARER_SHA256", "5d19bba3d8a3ad687a4c96e31a244f21cf88db2c8b39df2b42a399e57efafec1"):
            admitted = loader.admit_portable_payload(author)
        self.assertEqual(admitted["spec"]["payload_id"], "a311723a5a55c68dcfce022ccf5ebc7321d8bf058e8b73be7e7c3dd10096f4a1")
        self.assertEqual(len(admitted["pins"]), 13)
        self.assertFalse(self.manager.base.exists())

    def test_relative_roster_digest_alias_and_unadmitted_helper_refuse(self):
        for mutation in ("../escape", "/absolute", "C:/absolute", "catalog/../alias", "catalog\\alias", "catalog/a:stream"):
            spec = copy.deepcopy(self.spec)
            spec["files"][0]["path"] = mutation
            write_json(self.descriptor, spec)
            self.assertIsNone(self.manager._runtime(), mutation)
        for change in (lambda s: s.update(payload_id="0" * 64),
                       lambda s: s["files"].append(dict(s["files"][0])),
                       lambda s: s["files"][0].update(size=True),
                       lambda s: s.update(prepared_source={"path": "external"})):
            spec = copy.deepcopy(self.spec)
            change(spec)
            write_json(self.descriptor, spec)
            self.assertIsNone(self.manager._runtime())
        write_json(self.descriptor, self.spec)
        with patch.object(loader, "PORTABLE_PREPARER_SHA256", "0" * 64):
            self.assertIsNone(self.manager._runtime())
        self.assertFalse(self.manager.base.exists())

    def test_materialization_is_atomic_complete_owned_and_never_overwrites(self):
        admitted = loader.admit_portable_payload(self.bundle)
        runtime = self.manager._materialize(admitted, self.game)
        component = Path(runtime["portable"]["component_root"])
        self.assertEqual(component, self.state / "sideload" / "runtimes" / self.spec["payload_id"])
        self.assertEqual(Path(runtime["preparation_root"]), component / "background-epochs")
        self.assertEqual(runtime, self.manager._materialize(admitted, self.game))
        self.assertFalse(list(component.parent.glob(".stage-*")))
        target = component / "catalog/members-06.json"
        target.write_bytes(b"changed component retained")
        with self.assertRaises(loader.LoaderError):
            self.manager._materialize(admitted, self.game)
        self.assertEqual(target.read_bytes(), b"changed component retained")
        self.assertEqual((self.payload / "catalog/members-06.json").read_bytes(), b"invented never executed catalog/members-06.json")

    def test_partial_component_and_game_overlap_fail_without_copy_or_repair(self):
        admitted = loader.admit_portable_payload(self.bundle)
        component = self.manager.base / "runtimes" / self.spec["payload_id"]
        component.mkdir(parents=True)
        (component / "unknown.bin").write_bytes(b"unknown")
        with self.assertRaises((loader.LoaderError, FileNotFoundError)):
            self.manager._materialize(admitted, self.game)
        self.assertEqual(list(component.iterdir()), [component / "unknown.bin"])
        inside = loader.LoaderManager(self.game / "data")
        with self.assertRaisesRegex(loader.LoaderError, "outside"):
            inside._materialize(admitted, self.game)
        self.assertFalse(inside.base.exists())

    def test_failed_copy_retains_stage_and_does_not_publish_component(self):
        admitted = loader.admit_portable_payload(self.bundle)
        with patch.object(loader.shutil, "copyfileobj", side_effect=OSError("invented disk failure")):
            with self.assertRaisesRegex(OSError, "disk failure"):
                self.manager._materialize(admitted, self.game)
        root = self.manager.base / "runtimes"
        self.assertFalse((root / self.spec["payload_id"]).exists())
        self.assertEqual(len(list(root.glob(".stage-*"))), 1)

    def test_explicit_install_uses_normal_saved_pack_and_portable_epoch_then_preserves_failure(self):
        prepared = dict(schema="texturequest-direct-prepared-v1", id="tiny downloaded pack", archives=[])
        write_json(self.state / "direct-prepared.json", prepared)
        calls = []
        def refuse(runtime, game, saved, output, progress):
            calls.append((runtime, game, saved, output))
            raise ValueError("invented preparation failure")
        with patch.object(loader, "_read_run", return_value=None), patch.object(loader.core, "validate_game"), \
             patch.object(loader, "_prepare", side_effect=refuse), \
             patch.object(loader, "_spawn_watcher", side_effect=AssertionError("cannot launch failed preparation")):
            with self.assertRaisesRegex(ValueError, "preparation failure"):
                self.manager.install(str(self.game), prepared)
        runtime, game, saved, output = calls[0]
        self.assertEqual(saved, self.state / "direct-prepared.json")
        self.assertEqual(game, self.game)
        self.assertFalse({"prepared_source", "prepared_ready"} & set(runtime))
        self.assertTrue(output.is_relative_to(Path(runtime["portable"]["component_root"]) / "background-epochs"))
        record = self.manager._record(self.game)
        self.assertEqual(record["phase"], "failed")
        changed = copy.deepcopy(record)
        changed["runtime"]["preparation_root"] = str(self.folder / "foreign")
        write_json(self.manager._path(self.game), loader._sealed(changed))
        with self.assertRaisesRegex(loader.LoaderError, "paths changed"):
            self.manager._record(self.game)

    def test_legacy_adopted_record_remains_readable_without_new_helper_or_runtime(self):
        destination = self.folder / "adopted Desktop data"
        manager, record = self.legacy_record(state=destination)
        with patch.object(loader, "_live_status_bytes", return_value=None), patch.object(loader, "_read_run", return_value=None), \
             patch.object(loader, "_preparer", side_effect=AssertionError("never import helper for old status")), \
             patch.object(loader, "_pin", side_effect=AssertionError("never repin old native code")):
            self.assertEqual(manager._record(self.game), loader._sealed(record))
            self.assertTrue(manager._status(self.game)["installed"])

    def test_portable_record_cannot_drop_component_contract_or_use_external_preparation(self):
        runtime = self.manager._materialize(loader.admit_portable_payload(self.bundle), self.game)
        installation = "e" * 32
        epoch = Path(runtime["preparation_root"]) / installation
        record = dict(schema="tq-background-installation-v2", installation_id=installation, revision=1, phase="preparing",
                      game=str(self.game), game_key=self.manager._key(self.game), runtime=runtime,
                      epoch_root=str(epoch), status_dir=str(epoch / "watcher"), stop_event="Local\\TQWatch-" + installation,
                      run_name="TextureQuestLoader-" + self.manager._key(self.game),
                      prepared=dict(path=str(self.state / "direct-prepared.json")),
                      ready=dict(output=str(epoch / "pack"), ready_json=dict(path=str(epoch / "pack/READY.json")),
                                 manifest=dict(path=str(epoch / "pack/READY.bin")), bindings=dict(path=str(epoch / "BINDINGS.bin"))))
        write_json(self.manager._path(self.game), loader._sealed(record))
        self.assertEqual(self.manager._record(self.game), loader._sealed(record))
        for change in (lambda r: r["runtime"].pop("portable"),
                       lambda r: r.update(schema="tq-background-installation-v1"),
                       lambda r: r["prepared"].update(path=str(self.folder / "external.json")),
                       lambda r: r["ready"]["manifest"].update(path=str(self.folder / "foreign.bin"))):
            changed = copy.deepcopy(record)
            change(changed)
            write_json(self.manager._path(self.game), loader._sealed(changed))
            with self.assertRaises(loader.LoaderError):
                self.manager._record(self.game)

    def test_unique_owned_discovery_default_mode_and_unknown_fail_closed(self):
        manager, _ = self.legacy_record()
        with patch.object(loader, "_live_status_bytes", return_value=None), patch.object(loader, "_read_run", return_value=None):
            self.assertEqual(manager.installed_game(), self.game)
            self.assertEqual(app.initial_loader_game(manager, None), self.game)
            self.assertEqual(app.default_install_mode(manager._status(self.game)), "background")
            other_game = self.folder / "other game"
            other_game.mkdir()
            self.legacy_record(game=other_game)
            self.assertIsNone(manager.installed_game())
            write_json(manager.registrations / ("f" * 24 + ".json"), {"unknown": True})
            with self.assertRaises(loader.LoaderError):
                manager.installed_game()
        fake = MagicMock()
        self.assertEqual(app.initial_loader_game(fake, self.game), self.game)
        fake.installed_game.assert_not_called()
        self.assertEqual(app.default_install_mode({"installed": False}), "pak")

    def test_transfer_tombstone_blocks_new_and_existing_controllers_and_cache(self):
        self.manager.base.mkdir(parents=True)
        (self.manager.base / "CONTROL-TRANSFER.json").write_bytes(b"malformed is also a refusal")
        with self.assertRaisesRegex(loader.LoaderError, "Desktop TextureQuest"):
            loader.LoaderManager(self.state)
        self.assertTrue(self.manager.cache_in_use())
        with self.assertRaisesRegex(loader.LoaderError, "Desktop TextureQuest"):
            self.manager.uninstall(str(self.game))
        fake_app = object.__new__(app.TextureQuestApp)
        fake_app.loader_manager = None
        fake_app.loader_error = "constructor refused transfer"
        self.assertTrue(fake_app.cache_in_use())

    def test_builder_explicit_payload_resources_receipt_and_default_unavailable(self):
        release = dict(schema="texturequest-release-v3", version="1.3.7", texture_version="1.2.3", texture_count=1, archives=[])
        manifest = self.folder / "release.json"
        write_json(manifest, release)
        commands = []
        def fake_pyinstaller(command, **kwargs):
            commands.append(command)
            folder = Path(command[command.index("--distpath") + 1]) / "TextureQuest"
            internal = folder / "_internal"
            internal.mkdir(parents=True)
            (folder / "TextureQuest.exe").write_bytes(b"fake build leaf never executed")
            for index, flag in enumerate(command):
                if flag == "--add-data":
                    source, destination = command[index + 1].rsplit(":", 1)
                    source = Path(source)
                    if source.is_dir():
                        shutil.copytree(source, internal / destination)
                    else:
                        shutil.copy2(source, internal / source.name)
        for with_payload in (False, True):
            output = self.folder / ("fake with payload" if with_payload else "fake default")
            argv = ["build_installer.py", "--manifest", str(manifest), "--output-root", str(output), "--version", "1.4.0"]
            if with_payload:
                argv += ["--loader-runtime-dir", str(self.bundle)]
            with patch.object(sys, "argv", argv), patch.object(builder, "BUILD", builder.BUILD), patch.object(builder, "DEST", builder.DEST), \
                 patch("texturequest_release.load_manifest", return_value=release), \
                 patch.object(builder.subprocess, "run", side_effect=fake_pyinstaller), \
                 patch.object(builder.importlib.metadata, "distribution", return_value=MagicMock(version="invented", files=[])), redirect_stdout(io.StringIO()):
                builder.main()
            folder = output / "dist/TextureQuest"
            receipt = json.loads((folder / "BUILD-MANIFEST.json").read_text())
            self.assertEqual(receipt["texture_version"], "1.2.3")
            if with_payload:
                self.assertEqual(receipt["loader_runtime"]["payload_id"], self.spec["payload_id"])
                self.assertTrue(any(row["path"] == "_internal/sideload/catalog/members-06.json" for row in receipt["files"]))
                self.assertIn("dataclasses", commands[-1])
                self.assertIn("zlib", commands[-1])
            else:
                self.assertIsNone(receipt["loader_runtime"])
                self.assertFalse((folder / "_internal/texturequest-sideload-runtime.json").exists())
        self.assertFalse(self.manager.base.exists())


if __name__ == "__main__":
    unittest.main()
