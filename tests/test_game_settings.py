"""Tiny D-only command INIs; no actual game/process/settings access."""
from contextlib import contextmanager
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import texturequest_game_settings as settings
from texture_swapper import core

TEST_ROOT = Path(os.environ["TQ_SETTINGS_TEST_ROOT"]).absolute()
if TEST_ROOT.drive.casefold() != "d:" or not TEST_ROOT.is_relative_to(Path("D:/eq2 paks")):
    raise RuntimeError("Native-settings tests require an owned D fixture root")
TEST_ROOT.mkdir(parents=True, exist_ok=True)


class NativeSettings(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=TEST_ROOT)
        self.root = Path(self.temp.name)
        self.game = self.root / "Game"; self.game.mkdir()
        self.state = self.root / "data"; self.state.mkdir()
        self.closed_calls = 0
        self.manager = settings.GameSettings(self.state, closed=self.closed)
        self.path = self.game / "eq2_recent.ini"
        self.original = b"; own fixture\r\nr_test_ao 2\r\nunchanged \x80\xff\r\n"
        self.path.write_bytes(self.original)
        self.changes = {"eq2_recent.ini": {"r_test_ao": "0"}}
        self.addCleanup(self.cleanup)

    def cleanup(self):
        # Verify the exact resolved target before the library's recursive delete.
        if not self.root.resolve().is_relative_to(TEST_ROOT.resolve()):
            raise RuntimeError("Refuse escaped fixture cleanup")
        result = self._outcome.result
        failed = any(test is self for test, _ in result.failures + result.errors)
        if failed:
            self.temp._finalizer.detach()  # Preserve failed tiny fixture history.
        else:
            self.temp.cleanup()

    def closed(self): self.closed_calls += 1

    def journal(self):
        _, home = self.manager._home(self.game)
        return home, core._load(home / "current.json")

    def test_01_exact_backup_and_full_restore(self):
        self.assertEqual(self.manager.apply(self.game, self.changes, features=["ambient_occlusion"])["status"], "applied")
        home, record = self.journal(); row = record["files"]["eq2_recent.ini"]
        self.assertEqual((home / row["backup"]).read_bytes(), self.original)
        self.assertEqual(row["before"]["sha256"], settings._hash(self.original)["sha256"])
        self.assertNotEqual(row["before"]["identity"], row["after"]["identity"])
        self.assertEqual(self.manager.restore(self.game)["status"], "restored")
        self.assertEqual(self.path.read_bytes(), self.original)

    def test_02_duplicate_commands_and_comments_are_preserved(self):
        raw = b" r_test_ao\t2 ; first\r\nr_TEST_ao = 3\t# second\nother 7\r\n"
        self.path.write_bytes(raw); self.manager.apply(self.game, self.changes)
        self.assertEqual(self.path.read_bytes(), b" r_test_ao\t0 ; first\r\nr_TEST_ao = 0\t# second\nother 7\r\n")
        self.manager.restore(self.game); self.assertEqual(self.path.read_bytes(), raw)

    def test_03_utf8_bom_mixed_newlines_and_invalid_bytes(self):
        raw = b"\xef\xbb\xbf; own \xe2\x98\x83\r\nr_test_ao 2\nunknown \xff"
        self.path.write_bytes(raw); self.manager.apply(self.game, self.changes)
        self.assertEqual(self.path.read_bytes(), raw.replace(b"r_test_ao 2", b"r_test_ao 0"))
        self.manager.restore(self.game); self.assertEqual(self.path.read_bytes(), raw)

    def test_04_utf16_le_and_be_exact_roundtrip(self):
        for encoding, bom in (("utf-16-le", b"\xff\xfe"), ("utf-16-be", b"\xfe\xff")):
            raw = bom + "; own snowman \u2603\r\nr_test_ao 2\r\n".encode(encoding)
            self.path.write_bytes(raw); self.manager.apply(self.game, self.changes)
            self.assertEqual(self.path.read_bytes(), bom + "; own snowman \u2603\r\nr_test_ao 0\r\n".encode(encoding))
            self.manager.restore(self.game); self.assertEqual(self.path.read_bytes(), raw)

    def test_05_absent_file_created_then_removed(self):
        changes = {"eq2.ini": {"r_test_aa": "0"}}
        target = self.game / "eq2.ini"; self.assertFalse(target.exists())
        self.manager.apply(self.game, changes); self.assertEqual(target.read_bytes(), b"r_test_aa 0\r\n")
        self.manager.restore(self.game); self.assertFalse(target.exists())

    def test_06_user_unrelated_edit_survives_merge(self):
        self.manager.apply(self.game, self.changes)
        current = self.path.read_bytes() + b"new_user_setting 19\n"
        self.path.write_bytes(current)
        self.assertEqual(self.manager.restore(self.game)["status"], "restored")
        self.assertEqual(self.path.read_bytes(), self.original + b"new_user_setting 19\n")

    def test_07_changed_owned_value_is_preserved_with_conflict(self):
        self.manager.apply(self.game, self.changes)
        changed = self.path.read_bytes().replace(b"r_test_ao 0", b"r_test_ao 9")
        self.path.write_bytes(changed)
        result = self.manager.restore(self.game)
        self.assertEqual(result["status"], "restored_with_conflicts")
        self.assertEqual(result["conflicts"], [{"file": "eq2_recent.ini", "key": "r_test_ao", "reason": "owned_line_changed"}])
        self.assertEqual(self.path.read_bytes(), changed)

    def test_08_changed_duplicate_count_is_not_guessed(self):
        self.manager.apply(self.game, self.changes)
        changed = self.path.read_bytes() + b"r_test_ao 0\r\n"; self.path.write_bytes(changed)
        result = self.manager.restore(self.game)
        self.assertEqual(result["conflicts"][0]["reason"], "occurrence_count_changed")
        self.assertEqual(self.path.read_bytes(), changed)

    def test_09_absent_keys_and_missing_final_newline_merge(self):
        raw = b"r_test_ao 2"; self.path.write_bytes(raw)
        changes = {"eq2_recent.ini": {"r_test_ao": "0", "r_test_aa": "0"}}
        self.manager.apply(self.game, changes)
        self.path.write_bytes(b"; new user header\n" + self.path.read_bytes())
        self.manager.restore(self.game)
        self.assertEqual(self.path.read_bytes(), b"; new user header\n" + raw)

    def test_10_vendor_paths_and_multiline_values_refuse(self):
        for changes in ({"eq2_default.ini": {"r_test_ao": "0"}}, {"../eq2.ini": {"r_test_ao": "0"}}, {"eq2.ini": {"r_test_ao": "0\nr_light_priority -1"}}):
            with self.assertRaises(ValueError): self.manager.apply(self.game, changes)
        self.assertEqual(self.path.read_bytes(), self.original)

    def test_11_changed_target_cas_refuses_without_overwrite(self):
        original_commit = settings._commit
        def race(path, expected, raw, token):
            path.write_bytes(b"r_test_ao 9\r\n")
            return original_commit(path, expected, raw, token)
        with patch.object(settings, "_commit", side_effect=race), self.assertRaises(ValueError):
            self.manager.apply(self.game, self.changes)
        self.assertEqual(self.path.read_bytes(), b"r_test_ao 9\r\n")
        with self.assertRaisesRegex(ValueError, "Interrupted"): self.manager.restore(self.game)

    def test_12_backup_exists_and_matches_before_first_commit(self):
        original_commit = settings._commit
        def checked(path, expected, raw, token):
            home, record = self.journal()
            self.assertEqual((home / record["files"]["eq2_recent.ini"]["backup"]).read_bytes(), self.original)
            self.assertEqual(record["phase"], "applying")
            return original_commit(path, expected, raw, token)
        with patch.object(settings, "_commit", side_effect=checked): self.manager.apply(self.game, self.changes)

    def test_13_changed_backup_refuses_restore(self):
        self.manager.apply(self.game, self.changes); current = self.path.read_bytes()
        home, record = self.journal(); (home / record["files"]["eq2_recent.ini"]["backup"]).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "backup changed"): self.manager.restore(self.game)
        self.assertEqual(self.path.read_bytes(), current)

    def test_14_interrupted_after_atomic_write_can_restore_exact(self):
        save = core._save; count = 0
        def fail_after_write(path, value):
            nonlocal count
            count += 1
            if count == 2: raise OSError("new simulated journal interruption")
            return save(path, value)
        with patch.object(core, "_save", side_effect=fail_after_write), self.assertRaises(OSError):
            self.manager.apply(self.game, self.changes)
        self.assertEqual(self.manager.status(self.game)["status"], "applying")
        self.assertEqual(self.manager.restore(self.game)["status"], "restored")
        self.assertEqual(self.path.read_bytes(), self.original)

    def test_15_partial_two_file_restore_resumes_known_hashes(self):
        other = self.game / "eq2.ini"; original_other = b"r_test_aa 4\n"; other.write_bytes(original_other)
        self.manager.apply(self.game, {**self.changes, "eq2.ini": {"r_test_aa": "0"}})
        commit = settings._commit; calls = 0
        def fail_second(*args):
            nonlocal calls
            calls += 1
            if calls == 2: raise OSError("new partial restore interruption")
            return commit(*args)
        with patch.object(settings, "_commit", side_effect=fail_second), self.assertRaises(OSError):
            self.manager.restore(self.game)
        self.assertEqual(self.manager.status(self.game)["status"], "restoring")
        self.manager.restore(self.game)
        self.assertEqual(self.path.read_bytes(), self.original); self.assertEqual(other.read_bytes(), original_other)

    def test_16_idempotent_reinstall_keeps_original_backup(self):
        self.manager.apply(self.game, self.changes); _, first = self.journal()
        self.manager.apply(self.game, self.changes); _, second = self.journal()
        self.assertEqual(first["id"], second["id"])
        with self.assertRaises(ValueError): self.manager.apply(self.game, {"eq2_recent.ini": {"r_test_ao": "1"}})
        self.manager.restore(self.game); self.assertEqual(self.path.read_bytes(), self.original)

    def test_17_new_install_after_restore_backs_up_new_user_baseline(self):
        self.manager.apply(self.game, self.changes); self.manager.restore(self.game)
        raw = self.original + b"new user data\n"; self.path.write_bytes(raw)
        self.manager.apply(self.game, self.changes); self.manager.restore(self.game)
        self.assertEqual(self.path.read_bytes(), raw)

    def test_18_closed_guard_blocks_writes_and_restore(self):
        with patch.object(self.manager, "closed", side_effect=RuntimeError("open")), self.assertRaises(RuntimeError):
            self.manager.apply(self.game, self.changes)
        self.assertEqual(self.path.read_bytes(), self.original)
        self.manager.apply(self.game, self.changes); current = self.path.read_bytes()
        with patch.object(self.manager, "closed", side_effect=RuntimeError("open")), self.assertRaises(RuntimeError):
            self.manager.restore(self.game)
        self.assertEqual(self.path.read_bytes(), current)

    def test_19_both_normal_locks_held_for_commit(self):
        held = set(); real_lock = core._lock; commit = settings._commit
        @contextmanager
        def lock(path):
            with real_lock(path):
                held.add(Path(path))
                try: yield
                finally: held.remove(Path(path))
        def checked(*args):
            self.assertIn(self.state / "operation.lock", held)
            self.assertIn(self.state / "sideload/helper-owner.lock", held)
            return commit(*args)
        with patch.object(core, "_lock", side_effect=lock), patch.object(settings, "_commit", side_effect=checked):
            self.manager.apply(self.game, self.changes); self.manager.restore(self.game)

    def test_20_interrupted_native_temp_is_preserved(self):
        self.manager.apply(self.game, self.changes); _, record = self.journal()
        pending = self.game / (".eq2_recent.ini.tq-" + record["id"] + ".old")
        pending.write_bytes(b"preserve interrupted file")
        with self.assertRaisesRegex(ValueError, "Interrupted owned"): self.manager.restore(self.game)
        self.assertEqual(pending.read_bytes(), b"preserve interrupted file")

    def test_21_partial_restore_preserves_one_owned_edit_and_restores_another(self):
        raw = b"r_test_ao 2\r\nr_test_aa 4\r\n"; self.path.write_bytes(raw)
        self.manager.apply(self.game, {"eq2_recent.ini": {"r_test_ao": "0", "r_test_aa": "0"}})
        self.path.write_bytes(b"r_test_ao 9\r\nr_test_aa 0\r\n")
        result = self.manager.restore(self.game)
        self.assertEqual(result["status"], "restored_with_conflicts")
        self.assertEqual(self.path.read_bytes(), b"r_test_ao 9\r\nr_test_aa 4\r\n")

    def test_22_no_policy_changes_is_visibly_unmanaged(self):
        result = self.manager.apply(self.game, {}, features=["bloom"])
        self.assertEqual(result["status"], "unmanaged"); self.assertIn("bloom", result["unmanaged"])
        self.assertEqual(self.closed_calls, 0); self.assertEqual(self.path.read_bytes(), self.original)

    def test_23_restore_journal_keeps_full_before_after_identity_and_hash(self):
        self.manager.apply(self.game, self.changes); _, applied = self.journal()
        expected = applied["files"]["eq2_recent.ini"]["after"]
        self.manager.restore(self.game); _, restored = self.journal()
        step = restored["restore_steps"]["eq2_recent.ini"]
        self.assertEqual(step["before"], expected)
        self.assertEqual(step["after"]["sha256"], settings._hash(self.original)["sha256"])
        self.assertNotEqual(step["after"]["identity"], expected["identity"])
        self.assertEqual(step["planned_after"]["sha256"], step["after"]["sha256"])

    def test_24_partial_restore_keeps_completed_step_across_resume(self):
        other = self.game / "eq2.ini"; other.write_bytes(b"r_test_aa 4\n")
        self.manager.apply(self.game, {**self.changes, "eq2.ini": {"r_test_aa": "0"}})
        commit = settings._commit; calls = 0
        def fail_second(*args):
            nonlocal calls
            calls += 1
            if calls == 2: raise OSError("new journal-step partial failure")
            return commit(*args)
        with patch.object(settings, "_commit", side_effect=fail_second), self.assertRaises(OSError):
            self.manager.restore(self.game)
        _, interrupted = self.journal()
        first = interrupted["restore_steps"]["eq2_recent.ini"]
        self.assertIn("after", first); self.assertNotIn("after", interrupted["restore_steps"]["eq2.ini"])
        self.manager.restore(self.game); _, final = self.journal()
        self.assertEqual(final["restore_steps"]["eq2_recent.ini"], first)
        self.assertIn("after", final["restore_steps"]["eq2.ini"])


    def test_25_partial_merged_restore_resumes_acked_and_unacked_steps(self):
        for window in ("after_ack", "before_ack"):
            with self.subTest(window=window):
                self.path.write_bytes(b"r_test_ao 2\r\nr_test_aa 4\r\n")
                other = self.game / "eq2.ini"; other.write_bytes(b"r_test_shadow 7\n")
                self.manager.apply(self.game, {"eq2_recent.ini": {"r_test_ao": "0", "r_test_aa": "0"},
                                              "eq2.ini": {"r_test_shadow": "0"}})
                # Both later unrelated edits must survive; one owned edit is a
                # conflict and must remain reported across the interruption.
                self.path.write_bytes(b"r_test_ao 9\r\nr_test_aa 0\r\nuser_recent 31\n")
                other.write_bytes(b"r_test_shadow 0\nuser_ini 32\n")
                commit, save = settings._commit, core._save
                commits = 0
                def interrupted_commit(*args):
                    nonlocal commits
                    commits += 1
                    if window == "after_ack" and commits == 2: raise OSError("new second-file interruption")
                    return commit(*args)
                def interrupted_save(path, value):
                    if window == "before_ack" and value.get("phase") == "restoring" and commits == 1:
                        raise OSError("new commit-before-ACK interruption")
                    return save(path, value)
                with patch.object(settings, "_commit", side_effect=interrupted_commit), \
                     patch.object(core, "_save", side_effect=interrupted_save), self.assertRaises(OSError):
                    self.manager.restore(self.game)
                _, interrupted = self.journal()
                self.assertEqual(interrupted["phase"], "restoring")
                first_name = next(iter(interrupted["restore_steps"]))
                first = interrupted["restore_steps"][first_name]
                self.assertEqual("after" in first, window == "after_ack")
                result = self.manager.restore(self.game)
                self.assertEqual(result["status"], "restored_with_conflicts")
                self.assertEqual(result["conflicts"], [{"file": "eq2_recent.ini", "key": "r_test_ao", "reason": "owned_line_changed"}])
                self.assertEqual(self.path.read_bytes(), b"r_test_ao 9\r\nr_test_aa 4\r\nuser_recent 31\n")
                self.assertEqual(other.read_bytes(), b"r_test_shadow 7\nuser_ini 32\n")
                _, final = self.journal()
                self.assertTrue(all("after" in step for step in final["restore_steps"].values()))
                if window == "after_ack": self.assertEqual(final["restore_steps"][first_name], first)


if __name__ == "__main__": unittest.main(verbosity=2)
