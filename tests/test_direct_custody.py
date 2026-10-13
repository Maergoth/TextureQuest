"""Actual D-only Windows custody/commit races and retirement recovery."""
import copy
import os
from pathlib import Path
import unittest
from unittest.mock import patch

import test_direct as td
import texturequest_direct as d
import texturequest_release as release
from texturequest_custody import DirectoryCustody, FileCustody
from texture_swapper import core


class CustodyTests(unittest.TestCase):
    setUp = td.DirectTests.setUp
    opener = td.DirectTests.opener
    prepare = td.DirectTests.prepare
    legacy = td.DirectTests.legacy

    def new_version(self):
        old = self.prepare()
        self.old_blob = self.blobs["a.vpk"]
        self.blobs["a.vpk"] = td.vpk(b"new exact released content")
        self.manifest = copy.deepcopy(self.manifest)
        self.manifest.update(version="1.3.1", texture_version="1.2.1")
        self.manifest["archives"][0].update(bytes=len(self.blobs["a.vpk"]), sha256=td.sha(self.blobs["a.vpk"]),
                                           url=d.REPOSITORY + "/releases/download/v1.3.1/a.vpk")
        return old

    def pause_retired(self):
        self.new_version()
        def progress(message, *_):
            if message == "Committing downloaded a.vpk":
                raise release.Paused("own retirement pause")
        with self.assertRaises(release.Paused):
            self.prepare(progress=progress)
        folder = self.swapper.state_dir / "downloads"
        journal = core._load(folder / "a.vpk.retirement.json")
        self.assertEqual(journal["phase"], "old-retired")
        self.assertEqual(Path(journal["retired"]).read_bytes(), self.old_blob)
        self.assertFalse((folder / "a.vpk").exists())
        return folder, journal

    def blocked(self, operation):
        try:
            operation()
        except PermissionError:
            return True
        return False

    def test_checked_legacy_source_write_and_replacement_blocked(self):
        old = self.legacy()
        source = Path(old["archives"][0]["path"])
        other = self.folder / "foreign.vpk"
        other.write_bytes(td.vpk(b"unknown replacement"))
        blocked = []
        def progress(message, *_):
            if message == "Moving saved a.vpk":
                blocked.extend((self.blocked(lambda: source.write_bytes(b"unknown")), self.blocked(lambda: os.replace(other, source))))
        self.prepare(progress=progress)
        self.assertEqual(blocked, [True, True])
        self.assertTrue(other.exists())
        self.assertFalse(source.exists())
        self.assertEqual((self.swapper.state_dir / "downloads/a.vpk").read_bytes(), self.blobs["a.vpk"])

    def test_duplicate_target_kept_while_write_replace_delete_blocked(self):
        old = self.legacy()
        target = self.swapper.state_dir / "downloads/a.vpk"
        target.parent.mkdir()
        target.write_bytes(self.blobs["a.vpk"])
        original_id = target.stat().st_ino
        other = self.folder / "foreign.vpk"
        other.write_bytes(td.vpk(b"unknown replacement"))
        blocked = []
        def progress(message, *_):
            if message == "Moving saved a.vpk":
                blocked.extend((self.blocked(lambda: target.write_bytes(b"unknown")), self.blocked(lambda: os.replace(other, target)), self.blocked(target.unlink)))
        self.prepare(progress=progress)
        self.assertEqual(blocked, [True, True, True])
        self.assertEqual(target.stat().st_ino, original_id)
        self.assertFalse(Path(old["archives"][0]["path"]).exists())
        self.assertEqual(target.read_bytes(), self.blobs["a.vpk"])

    def test_new_migration_slot_appearance_never_overwritten(self):
        old = self.legacy()
        target = self.swapper.state_dir / "downloads/a.vpk"
        unknown = td.vpk(b"unknown slot created after source verification")
        def progress(message, *_):
            if message == "Moving saved a.vpk":
                target.write_bytes(unknown)
        with self.assertRaises(OSError):
            self.prepare(progress=progress)
        self.assertEqual(target.read_bytes(), unknown)
        self.assertEqual(Path(old["archives"][0]["path"]).read_bytes(), self.blobs["a.vpk"])

    def test_checked_pending_source_write_replace_blocked_before_commit(self):
        source = self.swapper.state_dir / "downloads/a.vpk.pending"
        other = self.folder / "foreign.vpk"
        other.write_bytes(td.vpk(b"unknown pending replacement"))
        blocked = []
        def progress(message, *_):
            if message == "Committing downloaded a.vpk":
                blocked.extend((self.blocked(lambda: source.write_bytes(b"unknown")), self.blocked(lambda: os.replace(other, source))))
        self.prepare(progress=progress)
        self.assertEqual(blocked, [True, True])
        self.assertTrue(other.exists())
        self.assertEqual((self.swapper.state_dir / "downloads/a.vpk").read_bytes(), self.blobs["a.vpk"])

    def test_new_download_slot_appearance_never_overwritten(self):
        target = self.swapper.state_dir / "downloads/a.vpk"
        unknown = td.vpk(b"unknown slot inserted before native rename")
        def progress(message, *_):
            if message == "Committing downloaded a.vpk":
                target.write_bytes(unknown)
        with self.assertRaises(OSError):
            self.prepare(progress=progress)
        self.assertEqual(target.read_bytes(), unknown)
        self.assertEqual(target.with_name("a.vpk.pending").read_bytes(), self.blobs["a.vpk"])

    def test_old_download_source_guard_blocks_mutation_before_retirement(self):
        self.new_version()
        target = self.swapper.state_dir / "downloads/a.vpk"
        blocked = []
        def progress(message, *_):
            if message == "Retiring saved a.vpk":
                blocked.append(self.blocked(lambda: target.write_bytes(b"unknown")))
        self.prepare(progress=progress)
        self.assertEqual(blocked, [True])
        self.assertEqual(target.read_bytes(), self.blobs["a.vpk"])
        self.assertFalse(list(target.parent.glob("*.retirement.json")))
        self.assertFalse(list(target.parent.glob(".*.retired-*.vpk")))

    def test_unknown_slot_after_old_retirement_preserves_all_three_identities(self):
        self.new_version()
        target = self.swapper.state_dir / "downloads/a.vpk"
        unknown = td.vpk(b"unknown inserted while old cache is safely retired")
        def progress(message, *_):
            if message == "Committing downloaded a.vpk":
                target.write_bytes(unknown)
        with self.assertRaises(OSError):
            self.prepare(progress=progress)
        binding = core._load(target.with_name("a.vpk.retirement.json"))
        self.assertEqual(target.read_bytes(), unknown)
        self.assertEqual(Path(binding["retired"]).read_bytes(), self.old_blob)
        self.assertEqual(Path(binding["partial"]).read_bytes(), self.blobs["a.vpk"])
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual(target.read_bytes(), unknown)
        self.assertEqual(Path(binding["retired"]).read_bytes(), self.old_blob)

    def test_retirement_pause_recovery_uses_same_checked_files_no_network(self):
        folder, binding = self.pause_retired()
        self.requests.clear()
        self.prepare()
        self.assertEqual(self.requests, [])
        self.assertEqual((folder / "a.vpk").read_bytes(), self.blobs["a.vpk"])
        self.assertFalse(Path(binding["retired"]).exists())
        self.assertFalse((folder / "a.vpk.retirement.json").exists())

    def test_post_native_commit_receipt_failure_keeps_old_until_verified_retry(self):
        self.new_version()
        original = core._save
        def fail(path, value):
            if Path(path).name == "a.vpk.retirement.json" and value.get("phase") == "new-committed":
                raise OSError("own new-commit receipt failure")
            return original(path, value)
        with patch.object(core, "_save", side_effect=fail), self.assertRaises(OSError):
            self.prepare()
        target = self.swapper.state_dir / "downloads/a.vpk"
        binding = core._load(target.with_name("a.vpk.retirement.json"))
        self.assertEqual(target.read_bytes(), self.blobs["a.vpk"])
        self.assertEqual(Path(binding["retired"]).read_bytes(), self.old_blob)
        self.requests.clear()
        self.prepare()
        self.assertEqual(self.requests, [])
        self.assertFalse(Path(binding["retired"]).exists())

    def test_changed_retired_bytes_preserved_on_recovery(self):
        folder, binding = self.pause_retired()
        retired = Path(binding["retired"])
        retired.write_bytes(td.vpk(b"unknown retired content"))
        unknown = retired.read_bytes()
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual(retired.read_bytes(), unknown)
        self.assertTrue(Path(binding["partial"]).exists())

    def test_same_bytes_replaced_retired_file_id_never_adopted_or_deleted(self):
        folder, binding = self.pause_retired()
        retired = Path(binding["retired"])
        alternate = self.folder / "same-bytes.vpk"
        alternate.write_bytes(self.old_blob)
        os.replace(alternate, retired)
        with self.assertRaisesRegex(ValueError, "identity"):
            self.prepare()
        self.assertEqual(retired.read_bytes(), self.old_blob)

    def test_same_bytes_replaced_new_file_id_refuses_recovery(self):
        folder, binding = self.pause_retired()
        partial = Path(binding["partial"])
        alternate = self.folder / "same-bytes.vpk"
        alternate.write_bytes(self.blobs["a.vpk"])
        os.replace(alternate, partial)
        with self.assertRaisesRegex(ValueError, "identity"):
            self.prepare()
        self.assertEqual(partial.read_bytes(), self.blobs["a.vpk"])
        self.assertEqual(Path(binding["retired"]).read_bytes(), self.old_blob)

    def test_source_target_parent_directory_swaps_blocked(self):
        self.legacy()
        state = self.swapper.state_dir
        alternate = state.with_name("renamed-unknown-state")
        blocked = []
        def progress(message, *_):
            if message == "Moving saved a.vpk":
                blocked.append(self.blocked(lambda: os.rename(state, alternate)))
        self.prepare(progress=progress)
        self.assertEqual(blocked, [True])
        self.assertTrue(state.exists())
        self.assertFalse(alternate.exists())

    def test_native_rename_is_nonreplacing_and_preserves_file_id(self):
        source, target = self.folder / "native-source", self.folder / "native-target"
        source.write_bytes(b"known")
        target.write_bytes(b"unknown")
        with DirectoryCustody(self.folder) as dirs, FileCustody(source) as held:
            initial = held.identity()
            with self.assertRaises(OSError):
                held.rename_new(target, dirs)
            self.assertEqual(held.identity(), initial)
        self.assertEqual(target.read_bytes(), b"unknown")
        target.unlink()
        with DirectoryCustody(self.folder) as dirs, FileCustody(source) as held:
            initial = held.identity()
            held.rename_new(target, dirs)
            self.assertEqual(held.identity(), initial)
        self.assertFalse(source.exists())
        self.assertEqual(target.read_bytes(), b"known")

    def test_complete_surviving_cache_and_receipts_kept_held_through_zip_retirement(self):
        package, path = td.DirectTests.legacy_package(self)
        blocked = []
        def progress(message, *_):
            if message.startswith("Reclaiming verified legacy download"):
                target = self.swapper.state_dir / "downloads/a.vpk"
                pointer = self.swapper.state_dir / "texturequest-prepared.json"
                blocked.extend((self.blocked(lambda: target.write_bytes(b"unknown cache")), self.blocked(lambda: pointer.write_bytes(b"unknown pointer"))))
        with patch.object(d, "LEGACY_PACKAGES", (package,)):
            ready = self.prepare(progress=progress)
        self.assertEqual(blocked, [True, True])
        self.assertFalse(path.exists())
        self.assertEqual(core._load(self.swapper.state_dir / "texturequest-prepared.json"), ready)
        self.assertEqual((self.swapper.state_dir / "downloads/a.vpk").read_bytes(), self.blobs["a.vpk"])

    def test_all_310_ready_files_can_remain_held_until_legacy_retirement(self):
        self.validation.stop()
        for number in range(308):
            name = f"bounded-{number:03}.vpk"
            blob = td.vpk(("own fixture " + str(number)).encode())
            self.blobs[name] = blob
            self.manifest["archives"].append(dict(name=name, bytes=len(blob), sha256=td.sha(blob), entries=1,
                                                  url=d.REPOSITORY + "/releases/download/v1.3.0/" + name))
        package, path = td.DirectTests.legacy_package(self)
        with patch.object(d, "LEGACY_PACKAGES", (package,)):
            ready = self.prepare()
        self.assertEqual(len(ready["archives"]), 310)
        self.assertEqual(len(list((self.swapper.state_dir / "downloads").glob("*.vpk"))), 310)
        self.assertFalse(path.exists())
        self.assertEqual(core._load(self.swapper.state_dir / "texturequest-prepared.json"), ready)

    def legacy_superseded(self):
        old = self.legacy()
        self.blobs["a.vpk"] = td.vpk(b"new selected detail")
        self.manifest["archives"][0].update(bytes=len(self.blobs["a.vpk"]), sha256=td.sha(self.blobs["a.vpk"]))
        row = old["archives"][0]
        admitted = {(row["name"].casefold(), row["bytes"], row["sha256"])}
        return old, admitted

    def test_known_superseded_legacy_vpk_reclaimed_after_new_cache_verified(self):
        old, admitted = self.legacy_superseded()
        source = Path(old["archives"][0]["path"])
        unknown = source.parent / "unknown.txt"
        unknown.write_bytes(b"preserve unknown")
        with patch.object(d, "LEGACY_ARCHIVES", admitted):
            ready = self.prepare()
        self.assertFalse(source.exists())
        self.assertEqual(unknown.read_bytes(), b"preserve unknown")
        self.assertTrue((source.parent / "manifest.json").exists())
        self.assertEqual((self.swapper.state_dir / "downloads/a.vpk").read_bytes(), self.blobs["a.vpk"])
        receipt = core._load(next((self.swapper.state_dir / "cache-migrations").glob("*-legacy-downloads.json")))
        self.assertEqual(receipt["files"][0]["kind"], "legacy-prepared-vpk")
        self.assertEqual(receipt["files"][0]["status"], "removed")
        self.assertEqual(core._load(self.swapper.state_dir / "texturequest-prepared.json"), ready)

    def test_custom_unpublished_legacy_bytes_are_not_automatically_reclaimed(self):
        old, _ = self.legacy_superseded()
        source = Path(old["archives"][0]["path"])
        before = source.read_bytes()
        with patch.object(d, "LEGACY_ARCHIVES", frozenset()):
            self.prepare()
        self.assertEqual(source.read_bytes(), before)

    def test_obsolete_legacy_pixels_and_receipt_deny_progress_mutation(self):
        old, admitted = self.legacy_superseded()
        source = Path(old["archives"][0]["path"])
        receipt = source.parent / "manifest.json"
        blocked = []
        def progress(message, *_):
            if message == "Reclaiming verified obsolete cache: a.vpk":
                blocked.extend((self.blocked(lambda: source.write_bytes(b"unknown pixels")), self.blocked(lambda: receipt.write_bytes(b"unknown receipt"))))
        with patch.object(d, "LEGACY_ARCHIVES", admitted):
            self.prepare(progress=progress)
        self.assertEqual(blocked, [True, True])
        self.assertFalse(source.exists())
        self.assertEqual(core._load(receipt), old)

    def test_previous_direct_row_omitted_from_new_inventory_is_reclaimed(self):
        previous = self.prepare()
        old_path = self.swapper.state_dir / "downloads/b.vpk"
        self.blobs["c.vpk"] = td.vpk(b"replacement selected archive")
        self.manifest.update(version="1.3.1", texture_version="1.2.1")
        self.manifest["archives"][1] = dict(name="c.vpk", bytes=len(self.blobs["c.vpk"]), sha256=td.sha(self.blobs["c.vpk"]),
                                           entries=1, url=d.REPOSITORY + "/releases/download/v1.3.1/c.vpk")
        self.prepare()
        self.assertFalse(old_path.exists())
        self.assertEqual((old_path.parent / "c.vpk").read_bytes(), self.blobs["c.vpk"])
        receipt = core._load(next((self.swapper.state_dir / "cache-migrations").glob("*-legacy-downloads.json")))
        self.assertEqual(receipt["files"][0]["kind"], "obsolete-direct-vpk")
        self.assertEqual(receipt["files"][0]["sha256"], previous["archives"][1]["sha256"])


if __name__ == "__main__":
    unittest.main()
