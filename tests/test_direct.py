"""Direct-cache custody, bounded resume, migration and install; D-only fixtures."""
import copy
import hashlib
import io
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_direct as direct
from texture_swapper import core
import texturequest_release as release
from test_release import vpk, sha


def fixture():
    blobs = {"a.vpk": vpk(b"direct ready pixels"), "b.vpk": vpk(b"other retained texture")}
    rows = [dict(name=name, bytes=len(blob), sha256=sha(blob), entries=1,
                 url=direct.REPOSITORY + "/releases/download/v1.3.0/" + name) for name, blob in blobs.items()]
    return blobs, dict(schema="texturequest-release-v3", version="1.3.0", texture_version="1.2.0", texture_count=23282, archives=rows)


class Response(io.BytesIO):
    def __init__(self, blob, start=0, status=206, url=None, headers=None):
        super().__init__(blob[start:] if status == 206 else blob)
        self.status = status
        self.url = url or "https://release-assets.githubusercontent.com/fixture"
        self.headers = headers or {"Content-Length": str(len(blob) - start if status == 206 else len(blob)),
                                   "Content-Range": f"bytes {start}-{len(blob)-1}/{len(blob)}"}
    def geturl(self):
        return self.url
    def getcode(self):
        return self.status


class DirectTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1] / "build"
        root.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="direct-", dir=root)
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.swapper = direct.DirectSwapper(self.folder / "data")
        self.blobs, self.manifest = fixture()
        # Small real VPK fixtures use the same strict metadata/path contract.
        validator = direct.validate_manifest
        self.validation = patch.object(direct, "validate_manifest", side_effect=lambda value, pinned=True: validator(value, pinned=False))
        self.validation.start()
        self.addCleanup(self.validation.stop)
        self.requests = []

    def opener(self, request, **_):
        name = request.full_url.rsplit("/", 1)[1]
        start = int(request.get_header("Range", "bytes=0-")[6:-1])
        self.requests.append((name, start))
        return Response(self.blobs[name], start, 206 if start else 200)

    def prepare(self, **kwargs):
        return direct.prepare_direct(self.swapper, self.manifest, opener=self.opener, **kwargs)

    def legacy(self):
        source = self.folder / "input"
        source.mkdir()
        paths = []
        for name, blob in self.blobs.items():
            path = source / name
            path.write_bytes(blob)
            paths.append(path)
        value = core.Swapper.prepare(self.swapper, paths)
        core._atomic(self.swapper.state_dir / "texturequest-prepared.json", value)
        return value

    def test_single_ready_cache_and_network_free_reverify(self):
        ready = self.prepare()
        self.assertEqual(ready["schema"], direct.SCHEMA)
        self.assertEqual({Path(row["path"]).parent for row in ready["archives"]}, {self.swapper.state_dir / "downloads"})
        self.assertFalse((self.swapper.state_dir / "prepared").exists())
        self.assertFalse(any(self.swapper.state_dir.rglob("*.zip")))
        self.requests.clear()
        self.prepare()
        self.assertEqual(self.requests, [])
        self.assertEqual(sum(p.stat().st_size for p in (self.swapper.state_dir / "downloads").glob("*.vpk")), ready["total_bytes"])

    def test_pause_resumes_verified_prefix_and_never_creates_second_dataset(self):
        pause = threading.Event()
        def notify(message, done, total):
            if message.startswith("Downloading"):
                pause.set()
        with patch.object(direct, "CHECKPOINT", 32), self.assertRaises(release.Paused):
            self.prepare(progress=notify, pause=pause.is_set)
        partial = self.swapper.state_dir / "downloads/a.vpk.pending"
        self.assertEqual(partial.stat().st_size, 32)
        self.assertFalse((self.swapper.state_dir / "texturequest-prepared.json").exists())
        pause.clear()
        self.prepare(pause=pause.is_set)
        self.assertIn(("a.vpk", 32), self.requests)
        self.assertFalse(partial.exists())

    def test_uncommitted_extra_partial_bytes_preserved(self):
        pause = threading.Event()
        with patch.object(direct, "CHECKPOINT", 32), self.assertRaises(release.Paused):
            self.prepare(progress=lambda m, *_: pause.set() if m.startswith("Downloading") else None, pause=pause.is_set)
        path = self.swapper.state_dir / "downloads/a.vpk.pending"
        with path.open("ab") as stream:
            stream.write(b"unknown")
        before = path.read_bytes()
        with self.assertRaisesRegex(ValueError, "length changed"):
            self.prepare()
        self.assertEqual(path.read_bytes(), before)

    def test_changed_checkpoint_prefix_preserved(self):
        pause = threading.Event()
        with patch.object(direct, "CHECKPOINT", 32), self.assertRaises(release.Paused):
            self.prepare(progress=lambda m, *_: pause.set() if m.startswith("Downloading") else None, pause=pause.is_set)
        path = self.swapper.state_dir / "downloads/a.vpk.pending"
        path.write_bytes(b"X" * 32)
        with self.assertRaisesRegex(ValueError, "prefix changed"):
            self.prepare()
        self.assertEqual(path.read_bytes(), b"X" * 32)

    def test_ignored_range_requires_same_saved_prefix(self):
        pause = threading.Event()
        with patch.object(direct, "CHECKPOINT", 32), self.assertRaises(release.Paused):
            self.prepare(progress=lambda m, *_: pause.set() if m.startswith("Downloading") else None, pause=pause.is_set)
        direct.prepare_direct(self.swapper, self.manifest, opener=lambda request, **_: Response(self.blobs[request.full_url.rsplit("/", 1)[1]], status=200))
        self.assertFalse((self.swapper.state_dir / "downloads/a.vpk.pending").exists())

    def test_unknown_files_and_malformed_journal_never_erased(self):
        folder = self.swapper.state_dir / "downloads"
        folder.mkdir()
        unknown = folder / "notes.zip"
        unknown.write_bytes(b"keep")
        journal = folder / "a.vpk.pending.json"
        journal.write_bytes(b'{"foreign":true}')
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual(unknown.read_bytes(), b"keep")
        self.assertEqual(journal.read_bytes(), b'{"foreign":true}')

    def test_unrecognized_current_archive_never_replaced(self):
        folder = self.swapper.state_dir / "downloads"
        folder.mkdir()
        path = folder / "a.vpk"
        path.write_bytes(vpk(b"unknown user content"))
        old = path.read_bytes()
        with self.assertRaisesRegex(ValueError, "Unrecognized"):
            self.prepare()
        self.assertEqual(path.read_bytes(), old)

    def test_recognized_old_version_replaced_only_after_full_new_verification(self):
        old = self.prepare()
        old_bytes = (self.swapper.state_dir / "downloads/a.vpk").read_bytes()
        self.manifest = copy.deepcopy(self.manifest)
        self.manifest.update(version="1.4.0", texture_version="1.3.0")
        self.blobs["a.vpk"] = vpk(b"new approved version")
        row = self.manifest["archives"][0]
        row.update(bytes=len(self.blobs["a.vpk"]), sha256=sha(self.blobs["a.vpk"]), url=direct.REPOSITORY + "/releases/download/v1.4.0/a.vpk")
        new = self.prepare()
        self.assertNotEqual(old["id"], new["id"])
        self.assertNotEqual((self.swapper.state_dir / "downloads/a.vpk").read_bytes(), old_bytes)
        self.assertEqual((self.swapper.state_dir / "downloads/b.vpk").read_bytes(), self.blobs["b.vpk"])

    def test_bad_download_preserves_old_cache_and_pending_evidence(self):
        self.prepare()
        target = self.swapper.state_dir / "downloads/a.vpk"
        original = target.read_bytes()
        row = self.manifest["archives"][0]
        row.update(sha256=sha(b"some different approved bytes"))
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.prepare()
        self.assertEqual(target.read_bytes(), original)
        self.assertTrue(target.with_name("a.vpk.pending").exists())

    def test_archive_index_must_match_after_full_download_hash(self):
        self.manifest["archives"][0]["entries"] = 2
        with self.assertRaisesRegex(ValueError, "index differs"):
            self.prepare()
        self.assertFalse((self.swapper.state_dir / "downloads/a.vpk").exists())

    def test_legacy_same_drive_move_and_exact_duplicate_coalesce(self):
        old = self.legacy()
        folder = self.swapper.state_dir / "downloads"
        folder.mkdir()
        (folder / "a.vpk").write_bytes(self.blobs["a.vpk"])
        ids = {row["name"]: Path(row["path"]).stat().st_ino for row in old["archives"]}
        ids["a.vpk"] = (folder / "a.vpk").stat().st_ino  # Preserve the held duplicate target.
        ready = self.prepare()
        self.assertEqual(self.requests, [])
        for row in ready["archives"]:
            self.assertEqual(Path(row["path"]).stat().st_ino, ids[row["name"]])
        self.assertTrue(all(not Path(row["path"]).exists() for row in old["archives"]))
        self.assertTrue((self.swapper.state_dir / "prepared" / old["id"] / "manifest.json").exists())
        self.assertEqual(len(list((self.swapper.state_dir / "cache-migrations").glob("*.json"))), 2)

    def test_complete_legacy_hashes_verified_before_first_move(self):
        old = self.legacy()
        second = Path(old["archives"][1]["path"])
        second.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.prepare()
        self.assertTrue(Path(old["archives"][0]["path"]).exists())
        self.assertFalse((self.swapper.state_dir / "downloads/a.vpk").exists())

    def test_migration_after_rename_interruption_recovers_known_target(self):
        old = self.legacy()
        atomic = core._atomic
        def crash(path, value):
            if Path(path).parent.name == "cache-migrations":
                raise OSError("simulated receipt interruption")
            return atomic(path, value)
        with patch.object(core, "_atomic", side_effect=crash), self.assertRaisesRegex(OSError, "interruption"):
            self.prepare()
        self.assertFalse(Path(old["archives"][0]["path"]).exists())
        self.assertTrue((self.swapper.state_dir / "downloads/a.vpk.migration.json").exists())
        self.prepare()
        self.assertEqual(self.requests, [])
        self.assertFalse((self.swapper.state_dir / "downloads/a.vpk.migration.json").exists())

    def test_operation_and_helper_locks_refuse_cache_change(self):
        for path in (self.swapper.state_dir / "operation.lock", self.swapper.state_dir / "sideload/helper-owner.lock"):
            with core._lock(path), self.assertRaises(RuntimeError):
                self.prepare()
        with self.assertRaises(RuntimeError):
            self.prepare(cache_in_use=lambda: True)
        self.assertEqual(self.requests, [])

    def test_path_links_and_hardlinks_refused(self):
        folder = self.swapper.state_dir / "downloads"
        folder.mkdir()
        outside = self.folder / "outside.vpk"
        outside.write_bytes(self.blobs["a.vpk"])
        os.link(outside, folder / "a.vpk")
        with self.assertRaisesRegex(ValueError, "Hard-linked"):
            self.prepare()
        self.assertEqual(outside.read_bytes(), self.blobs["a.vpk"])

    def test_distinct_direct_receipt_never_allows_path_override(self):
        ready = self.prepare()
        ready["archives"][0]["path"] = str(self.folder / "outside.vpk")
        core._save(self.swapper.state_dir / "direct-prepared.json", ready)
        with self.assertRaisesRegex(ValueError, "inventory changed"):
            self.swapper._prepared(ready)

    def test_explicit_install_reuses_existing_core_guards_and_fixture_staging(self):
        ready = self.prepare()
        game = self.folder / "fixture-game"
        (game / "paks").mkdir(parents=True)
        (game / "EverQuest2.exe").write_bytes(b"own fixture")
        for name in self.blobs:
            (game / "paks" / name).write_bytes(vpk(b"native fixture"))
        with patch.object(core, "ensure_game_closed", side_effect=RuntimeError("game active")), self.assertRaises(RuntimeError):
            self.swapper.install(ready, game)
        with patch.object(core, "ensure_game_closed", return_value=None):
            installed = self.swapper.install(ready, game)
        self.assertEqual(installed["status"], "installed")
        for name, blob in self.blobs.items():
            self.assertEqual((game / "paks" / name).read_bytes(), blob)
        with core._lock(self.swapper.state_dir / "sideload/helper-owner.lock"), self.assertRaises(RuntimeError):
            self.swapper.install(ready, game)

    def test_mutated_prepared_archive_refuses_install_before_target_write(self):
        ready = self.prepare()
        game = self.folder / "fixture-game"
        (game / "paks").mkdir(parents=True)
        (game / "EverQuest2.exe").write_bytes(b"own fixture")
        for name in self.blobs:
            (game / "paks" / name).write_bytes(vpk(b"native fixture"))
        before = {p.name: p.read_bytes() for p in (game / "paks").iterdir()}
        Path(ready["archives"][0]["path"]).write_bytes(b"changed")
        with patch.object(core, "ensure_game_closed", return_value=None), self.assertRaises(ValueError):
            self.swapper.install(ready, game)
        self.assertEqual(before, {p.name: p.read_bytes() for p in (game / "paks").iterdir()})

    def test_range_and_redirect_mismatch_preserve_partial(self):
        for response in (Response(self.blobs["a.vpk"], headers={"Content-Length": "1", "Content-Range": "bad"}),
                         Response(self.blobs["a.vpk"], url="https://foreign.example/asset")):
            with self.assertRaises(ValueError):
                direct.prepare_direct(self.swapper, self.manifest, opener=lambda *_a, **_k: response)
        self.assertFalse((self.swapper.state_dir / "downloads/a.vpk").exists())

    def legacy_package(self, blob=b"exact immutable legacy container"):
        package = dict(version="1.0.0", name="Texture-Pack.zip", bytes=len(blob), sha256=sha(blob))
        path = self.swapper.state_dir / "downloads/1.0.0/Texture-Pack.zip"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
        return package, path

    def test_legacy_zip_retired_only_after_complete_direct_pointer_commits(self):
        package, path = self.legacy_package()
        external = self.folder / "external.zip"
        external.write_bytes(b"never owned")
        partial = path.with_name(path.name + ".download")
        partial.write_bytes(b"partial")
        with patch.object(direct, "LEGACY_PACKAGES", (package,)):
            ready = self.prepare()
        self.assertFalse(path.exists())
        self.assertEqual(external.read_bytes(), b"never owned")
        self.assertEqual(partial.read_bytes(), b"partial")
        self.assertEqual(core._load(self.swapper.state_dir / "texturequest-prepared.json"), ready)
        receipts = list((self.swapper.state_dir / "cache-migrations").glob("*-legacy-downloads.json"))
        self.assertEqual(len(receipts), 1)
        receipt = core._load(receipts[0])
        self.assertEqual(receipt["removed_bytes"], package["bytes"])
        self.assertEqual(receipt["files"][0]["status"], "removed")

    def test_mismatched_and_unknown_legacy_downloads_preserved(self):
        package, path = self.legacy_package()
        path.write_bytes(b"different user bytes")
        unknown = path.parent / "unlisted.zip"
        unknown.write_bytes(b"unknown")
        with patch.object(direct, "LEGACY_PACKAGES", (package,)):
            self.prepare()
        self.assertEqual(path.read_bytes(), b"different user bytes")
        self.assertEqual(unknown.read_bytes(), b"unknown")
        result = core._load(next((self.swapper.state_dir / "cache-migrations").glob("*-legacy-downloads.json")))
        self.assertEqual(result["files"][0]["status"], "preserved-mismatch")

    def test_mutated_complete_direct_cache_blocks_legacy_zip_removal(self):
        package, path = self.legacy_package()
        original = direct._retire_legacy_downloads
        def mutate(swapper, prepared, *args):
            Path(prepared["archives"][0]["path"]).write_bytes(b"changed before cleanup")
            return original(swapper, prepared, *args)
        with patch.object(direct, "LEGACY_PACKAGES", (package,)), patch.object(direct, "_retire_legacy_downloads", side_effect=mutate), self.assertRaises(ValueError):
            self.prepare()
        self.assertTrue(path.exists())

    def test_interrupted_cleanup_retry_preserves_intent_and_finishes_known_file(self):
        package, path = self.legacy_package()
        with patch.object(direct, "LEGACY_PACKAGES", (package,)), patch.object(direct, "_remove_exact_download", side_effect=release.Paused("fixture pause")), self.assertRaises(release.Paused):
            self.prepare()
        initial = next((self.swapper.state_dir / "cache-migrations").glob("*-legacy-downloads.json"))
        raw = initial.read_bytes()
        self.assertTrue(path.exists())
        with patch.object(direct, "LEGACY_PACKAGES", (package,)):
            self.prepare()
        self.assertFalse(path.exists())
        self.assertEqual(initial.read_bytes(), raw)
        self.assertEqual(len(list((self.swapper.state_dir / "cache-migrations").glob("*-legacy-downloads.json"))), 2)

    def test_windows_retained_delete_handle_denies_mutation_during_hash(self):
        package, path = self.legacy_package()
        hash_stream = release.hash_stream
        witnessed = []
        def checked(stream, *args, **kwargs):
            try:
                path.write_bytes(b"unexpected mutation")
            except PermissionError:
                witnessed.append(True)
            else:
                witnessed.append(False)
            return hash_stream(stream, *args, **kwargs)
        with patch.object(release, "hash_stream", side_effect=checked):
            self.assertTrue(direct._remove_exact_download(path, package, None))
        self.assertEqual(witnessed, [True])
        self.assertFalse(path.exists())

    def test_legacy_version_download_lock_prevents_reclamation(self):
        package, path = self.legacy_package()
        with core._lock(path.parent / "download.lock"), patch.object(direct, "LEGACY_PACKAGES", (package,)), self.assertRaises(RuntimeError):
            self.prepare()
        self.assertTrue(path.exists())


class ManifestTests(unittest.TestCase):
    def test_initial_complete_inventory_same_bytes_as_verified_1_2(self):
        path = Path(r"D:\eq2 paks\work\texturequest_release_raid_v1\publisher-v2\runs\initial_v2\release-manifest.json")
        base = core._json(path.read_bytes())
        manifest = core._json(Path(r"D:\eq2 paks\work\texturequest_direct_release_v1\release-manifest.json").read_bytes())
        self.assertIs(direct.validate_manifest(manifest), manifest)
        self.assertEqual(len(manifest["archives"]), 310)
        self.assertEqual(manifest["texture_count"], 23282)
        self.assertEqual([(r["name"], r["bytes"], r["sha256"]) for r in manifest["archives"]],
                         [(r["name"], r["bytes"], r["sha256"]) for r in base["archives"]])
        self.assertLess(max(r["bytes"] for r in manifest["archives"]), 2**31)

    def test_immutable_prior_direct_urls_reused_but_foreign_future_rejected(self):
        _, manifest = fixture()
        manifest["version"] = "1.4.0"
        direct.validate_manifest(manifest, pinned=False)
        for url in (direct.REPOSITORY + "/releases/download/v1.5.0/a.vpk", "https://foreign.example/a.vpk", direct.REPOSITORY + "/releases/download/v1.2.0/a.vpk"):
            bad = copy.deepcopy(manifest)
            bad["archives"][0]["url"] = url
            with self.assertRaises(ValueError):
                direct.validate_manifest(bad, pinned=False)

    def test_duplicates_bools_unsafe_names_wrong_count_fail_closed(self):
        _, good = fixture()
        for change in (lambda m: m["archives"].append(dict(m["archives"][0])),
                       lambda m: m["archives"][0].update(bytes=True),
                       lambda m: m["archives"][0].update(name="../a.vpk"),
                       lambda m: m.update(texture_version="1.4.0")):
            bad = copy.deepcopy(good)
            change(bad)
            with self.assertRaises(ValueError):
                direct.validate_manifest(bad, pinned=False)
        with self.assertRaisesRegex(ValueError, "complete"):
            direct.validate_manifest(good)


if __name__ == "__main__":
    unittest.main()
