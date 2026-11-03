"""App-only refresh reuse with tiny genuine D channel/cache fixtures."""
import copy
import hashlib
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import texturequest_setup as setup
import texturequest_channels as channels
from texture_swapper import core
from texturequest_release import Paused
from test_channels import fixture

ROOT = Path(os.environ["TQ_REFRESH_REUSE_TEST_ROOT"]).absolute()
if ROOT.drive.casefold() != "d:" or not ROOT.is_relative_to(Path("D:/eq2 paks")):
    raise RuntimeError("Require an owned D refresh-reuse test root")
ROOT.mkdir(parents=True, exist_ok=True)


class RefreshReuse(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT)
        self.root = Path(self.temp.name)
        self.game, self.source, self.index, _, self.choices, opener, _ = fixture(self.root, (4,))
        self.store = channels.ChannelStore(self.root / "data")
        self.prepared = self.store.prepare(self.source, self.game, self.choices, opener=opener)
        self.graphics = MagicMock()
        self.graphics.status.return_value = dict(installed=True, installed_newer_app=False)
        self.upgrade = MagicMock(state=self.store.state_dir)
        self.upgrade.needed.return_value = True
        self.upgrade.snapshot.return_value = {"graphics": {"id": "own-old-hook"}}
        self.reports, self.order = [], []
        self.upgrade.before.side_effect = lambda *a, **kw: self.order.append("retire")
        self.graphics.install.side_effect = lambda *a, **kw: self.order.append("install")
        self.upgrade.complete.side_effect = lambda *a, **kw: self.order.append("cleanup")
        self.closed = patch.object(setup, "ensure_closed"); self.closed.start()
        self.addCleanup(self.closed.stop); self.addCleanup(self.cleanup)

    def cleanup(self):
        if not self.root.resolve().is_relative_to(ROOT.resolve()): raise RuntimeError("Escaped test cleanup")
        result = self._outcome.result
        if any(test is self for test, _ in result.failures + result.errors): self.temp._finalizer.detach()
        else: self.temp.cleanup()

    def saved(self, **changes):
        value = copy.deepcopy(self.prepared); value.update(changes)
        core._save(self.store.state_dir / "dds-prepared.json", value)
        return value

    def candidate(self, **changes):
        args = dict(game=self.game, source=self.source, choices=self.choices, store=self.store,
                    state=self.store.state_dir, in_use=lambda: False, pause=None)
        args.update(changes)
        return setup._saved_runtime_refresh(**args)

    def refresh(self, **changes):
        args = dict(minimum="1.3.0", binding={"vk": 106, "modifiers": 0}, in_use=lambda: False,
                    upgrading=True, report=self.reports.append)
        args.update(changes)
        fetch = MagicMock(return_value=self.source)
        result = setup.prepare_graphics(self.game, fetch, self.choices, self.store, self.graphics, self.upgrade, **args)
        fetch.assert_called_once(); return result

    def test_01_exact_app_refresh_checks_metadata_before_retire_without_dds_scan(self):
        verify = self.store.verify_for_install
        def checked(*args, **kwargs):
            self.order.append("metadata")
            return verify(*args, **kwargs)
        with patch.object(self.store, "prepare", side_effect=AssertionError("Unexpected PAK rescan")), \
             patch.object(channels, "read_sources", side_effect=AssertionError("Unexpected source read")), \
             patch.object(self.store, "verify", side_effect=AssertionError("Unexpected DDS scan")), \
             patch.object(self.store, "verify_for_install", side_effect=checked):
            _, prepared = self.refresh()
        self.assertEqual(prepared, self.prepared)
        self.assertEqual(self.order, ["metadata", "retire", "install", "cleanup"])
        self.assertTrue(any("coverage has not been rescanned" in message for message in self.reports))

    def test_02_explicit_normal_verification_still_calls_prepare(self):
        with patch.object(self.store, "prepare", wraps=self.store.prepare) as prepare:
            self.refresh(upgrading=False)
        prepare.assert_called_once()

    def test_03_complete_source_identity_change_never_reuses(self):
        changed = dict(self.source, new_release_attribute="different")
        with self.assertRaisesRegex(ValueError, "Verify / Update"):
            self.candidate(source=changed)

    def test_04_exact_game_choices_index_distribution_required(self):
        for changes in ({"game": str(self.root / "other")}, {"choices_sha256": "e" * 64},
                        {"index_sha256": "f" * 64}):
            self.saved(**changes)
            with self.assertRaisesRegex(ValueError, "Verify / Update"):
                self.candidate()
        for changes in ({"distribution": "legacy-full-dds"}, {"texture_version": "1.2.9"}):
            self.saved(**changes)
            self.assertIsNone(self.candidate())

    def test_05_same_version_missing_index_requests_explicit_repair_without_rescan(self):
        path = self.store.base / "metadata" / (self.source["index"]["sha256"] + ".json")
        path.unlink()
        with patch.object(self.store, "prepare", side_effect=AssertionError("Unexpected automatic repair")) as prepare, \
             self.assertRaisesRegex(ValueError, "Verify / Update"):
            self.refresh()
        prepare.assert_not_called(); self.upgrade.before.assert_not_called()

    def test_06_unchanged_refresh_trusts_commit_but_explicit_verify_detects_corrupt_dds(self):
        row = self.store.object_rows(self.prepared)[0]
        path = self.store.objects / row["entry"]; raw = path.read_bytes()
        path.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))
        with patch.object(self.store, "prepare", side_effect=AssertionError("No implicit recovery")):
            self.refresh()
        self.upgrade.before.assert_called_once(); self.graphics.install.assert_called_once()
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.store.verify(self.prepared)

    def test_07_cached_roster_must_bind_exact_current_recipe_targets(self):
        rows = self.store.object_rows(self.prepared)
        rows[0]["sha256"] = "d" * 64; rows[0]["entry"] = "dds/" + "d" * 64 + ".dds"
        self.saved(objects=self.store._metadata(rows))
        with self.assertRaisesRegex(ValueError, "Verify / Update"):
            self.candidate()

    def test_08_map_header_source_binding_must_match(self):
        raw = bytearray(Path(self.prepared["map"]["path"]).read_bytes()); raw[20] ^= 1
        path = self.store.base / "maps" / (hashlib.sha256(raw).hexdigest() + ".bin"); path.write_bytes(raw)
        self.saved(map=dict(path=str(path), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
        with self.assertRaisesRegex(ValueError, "Verify / Update"):
            self.candidate()

    def test_09_patched_game_archive_does_not_claim_new_coverage(self):
        (self.game / "paks/test.vpk").write_bytes(b"invented changed patch bytes; must never be opened here")
        with patch.object(self.store, "prepare", side_effect=AssertionError("Unexpected rescan")):
            self.refresh()
        self.assertTrue(any("saved texture matches" in message for message in self.reports))

    def test_10_missing_game_executable_refuses_before_retirement(self):
        (self.game / "EverQuest2.exe").unlink()
        with self.assertRaisesRegex(ValueError, "EverQuest2.exe"): self.refresh()
        self.upgrade.before.assert_not_called()

    def test_11_in_use_guard_is_preserved(self):
        with self.assertRaisesRegex(ValueError, "Close EQ2"): self.refresh(in_use=lambda: True)
        self.upgrade.before.assert_not_called()

    def test_12_pointer_race_after_metadata_admission_refuses_retirement(self):
        verify = self.store.verify_for_install
        def race(*args, **kwargs):
            result = verify(*args, **kwargs)
            self.saved(source_sha256="e" * 64)
            return result
        with patch.object(self.store, "verify_for_install", side_effect=race), self.assertRaisesRegex(ValueError, "preparation changed"):
            self.refresh()
        self.upgrade.before.assert_not_called()

    def test_13_current_recipe_profile_validation_is_required(self):
        with patch.object(channels.codec, "PROFILES", {}):
            with self.assertRaisesRegex(ValueError, "Verify / Update"):
                self.candidate()

    def test_14_pause_during_reuse_is_not_swallowed(self):
        calls = 0
        def pause():
            nonlocal calls
            calls += 1; return calls >= 2
        with self.assertRaises(Paused): self.refresh(pause=pause)
        self.upgrade.before.assert_not_called()


if __name__ == "__main__": unittest.main(verbosity=2)
