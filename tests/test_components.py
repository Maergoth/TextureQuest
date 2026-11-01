"""Component contracts and application CAS on invented D-only metadata."""
from pathlib import Path
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_components as components
from texture_swapper import core


class Components(unittest.TestCase):
    def setUp(self):
        parent = Path(__file__).resolve().parents[1] / "build/component-fixtures"
        parent.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.state = self.root / "state"
        self.game = self.root / "unopened game ü"

    def effects(self):
        return dict(components.normalize(), effects=True)

    def record_path(self):
        return next((self.state / "components").glob("*.json"))

    def saved_record(self):
        return core._load(self.record_path())

    def test_default_and_effect_selection_strict_and_copied(self):
        expected = dict(schema="texturequest-components-v1", textures=True, dlss=False, effects=False)
        self.assertEqual(components.normalize(), expected)
        value = self.effects()
        result = components.normalize(value)
        self.assertEqual(result, value)
        result["effects"] = False
        self.assertTrue(value["effects"])

    def test_unknown_fields_and_coercions_refuse(self):
        for value in ({}, [], "textures", dict(components.normalize(), extra=False),
                      dict(components.normalize(), schema="v2")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                components.normalize(value)
        for key in ("textures", "dlss", "effects"):
            for value in (0, 1, "false", None, []):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    components.normalize(dict(components.normalize(), **{key: value}))
        with self.assertRaisesRegex(ValueError, "required"):
            components.normalize(dict(components.normalize(), textures=False))
        self.assertTrue(components.normalize(dict(components.normalize(), dlss=True))['dlss'])

    def test_absent_load_does_not_create_state_or_read_game(self):
        expected = dict(selection=components.normalize(), record_sha256=None)
        original = core._plain
        def only_state(path, root=None):
            self.assertNotIn("unopened game", str(path))
            return original(path, root)
        with patch.object(core, "_plain", side_effect=only_state):
            self.assertEqual(components.load(self.state, self.game), expected)
        self.assertFalse(self.game.exists())
        self.assertFalse(self.state.exists())

    def test_round_trip_checksum_and_missing_record_defaults(self):
        before = components.load(self.state, self.game)
        saved = components.save(self.state, self.game, self.effects(), expected_before=before)
        self.assertEqual(saved, components.load(self.state, self.game))
        self.assertTrue(saved["selection"]["effects"])
        record = self.saved_record()
        self.assertEqual(record["revision"], 1)
        self.assertEqual(record["game"], os.path.normcase(os.path.abspath(self.game)))
        self.assertEqual(record["record_sha256"], core._digest({k:v for k,v in record.items() if k != "record_sha256"}))
        self.assertFalse(self.game.exists())

    def test_stale_snapshot_and_none_do_not_overwrite_changed_value(self):
        initial = components.load(self.state, self.game)
        components.save(self.state, self.game, self.effects(), expected_before=initial)
        raw = self.record_path().read_bytes()
        for stale in (initial, None):
            with self.subTest(stale=stale), self.assertRaisesRegex(ValueError, "changed in another"):
                components.save(self.state, self.game, components.normalize(), expected_before=stale)
            self.assertEqual(self.record_path().read_bytes(), raw)

    def test_game_paths_are_distinct_and_normalized_as_text(self):
        components.save(self.state, self.game, self.effects(), expected_before=None)
        self.assertFalse(components.load(self.state, self.root / "another game")["selection"]["effects"])
        equivalent = str(self.game / ".." / self.game.name)
        self.assertEqual(components.load(self.state, equivalent), components.load(self.state, self.game))
        if os.name == "nt":
            self.assertEqual(components.load(self.state, str(self.game).upper()), components.load(self.state, self.game))

    def test_aba_revision_and_idempotent_current_selection(self):
        first = components.save(self.state, self.game, self.effects(), expected_before=None)
        raw = self.record_path().read_bytes()
        same = components.save(self.state, self.game, self.effects(), expected_before=first)
        self.assertEqual(first, same)
        self.assertEqual(self.record_path().read_bytes(), raw)
        second = components.save(self.state, self.game, components.normalize(), expected_before=first)
        third = components.save(self.state, self.game, self.effects(), expected_before=second)
        self.assertNotEqual(first["record_sha256"], third["record_sha256"])
        self.assertEqual(self.saved_record()["revision"], 3)
        with self.assertRaisesRegex(ValueError, "changed in another"):
            components.save(self.state, self.game, components.normalize(), expected_before=first)

    def test_malformed_checksum_and_unknown_record_preserved(self):
        components.save(self.state, self.game, self.effects(), expected_before=None)
        record = self.saved_record()
        record["selection"]["effects"] = False
        self.record_path().write_text(json.dumps(record), encoding="utf-8")
        raw = self.record_path().read_bytes()
        for action in (lambda: components.load(self.state, self.game),
                       lambda: components.save(self.state, self.game, self.effects(), expected_before=None)):
            with self.assertRaisesRegex(ValueError, "checksum"):
                action()
            self.assertEqual(self.record_path().read_bytes(), raw)
        record["unknown"] = "new"
        core._save(self.record_path(), record)
        raw = self.record_path().read_bytes()
        with self.assertRaisesRegex(ValueError, "unsupported"):
            components.load(self.state, self.game)
        self.assertEqual(self.record_path().read_bytes(), raw)

    def test_null_selection_and_nonobject_record_are_not_defaults(self):
        components.save(self.state, self.game, self.effects(), expected_before=None)
        record = self.saved_record()
        record["selection"] = None
        core._save(self.record_path(), record)
        raw = self.record_path().read_bytes()
        with self.assertRaisesRegex(ValueError, "unsupported"):
            components.load(self.state, self.game)
        self.assertEqual(self.record_path().read_bytes(), raw)
        self.record_path().write_bytes(b"[]\n")
        with self.assertRaisesRegex(ValueError, "Malformed"):
            components.load(self.state, self.game)
        self.assertEqual(self.record_path().read_bytes(), b"[]\n")

    def test_existing_lock_blocks_save_and_atomic_failure_preserves_old(self):
        before = components.save(self.state, self.game, components.normalize(), expected_before=None)
        raw = self.record_path().read_bytes()
        lock = next((self.state / "components").glob("*.lock"))
        with core._lock(lock), self.assertRaisesRegex(RuntimeError, "active"):
            components.save(self.state, self.game, self.effects(), expected_before=before)
        with patch.object(core, "_atomic", side_effect=OSError("invented write failure")):
            with self.assertRaisesRegex(OSError, "invented"):
                components.save(self.state, self.game, self.effects(), expected_before=before)
        self.assertEqual(self.record_path().read_bytes(), raw)

    def test_invalid_snapshot_or_game_has_no_writes_and_dlss_preference_is_allowed(self):
        for value in (True, {}, dict(selection=self.effects(), record_sha256=None),
                      dict(selection=None, record_sha256=None),
                      dict(selection=components.normalize(), record_sha256="bad")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                components.save(self.state, self.game, self.effects(), expected_before=value)
        for game in (None, "", "relative game", b"bytes", "D:\\bad\x00path", "D:\\" + "x" * 3000):
            with self.subTest(game=game), self.assertRaises(ValueError):
                components.load(self.state, game)
        self.assertFalse(self.state.exists())
        saved = components.save(self.state, self.game, dict(components.normalize(), dlss=True), expected_before=None)
        self.assertTrue(saved['selection']['dlss'])
        self.assertFalse(self.game.exists())


if __name__ == "__main__":
    unittest.main()
