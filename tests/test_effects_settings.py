from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from texturequest_effects import prepare_effects_config, restore_effects_config


PATH = r"D:\TextureQuest\effects"


class EffectsSettingsTests(unittest.TestCase):
    def test_existing_user_config_roundtrip_with_bom_unicode_and_escaped_comma(self):
        raw = ("\ufeff; Keep my effects\r\n[GENERAL]\r\nEffectSearchPaths = C:\\Café,,Room\\Shaders\\**,D:\\Mine\r\n"
               "NoReloadOnInit = 1 ; startup note\r\nSkipLoadingDisabledEffects=1\r\nPresetPath=Mine.ini\r\n"
               "[OVERLAY]\r\nTutorialProgress=4\r\n").encode("utf-8")
        prepared, lease = prepare_effects_config(raw, r"D:\TQ,Authored\effects")
        self.assertIn(b"C:\\Caf\xc3\xa9,,Room\\Shaders\\**,D:\\Mine,D:\\TQ,,Authored\\effects", prepared)
        self.assertIn(b"NoReloadOnInit = 0 ; startup note\r\n", prepared)
        self.assertIn(b"PresetPath=Mine.ini\r\n", prepared)
        self.assertTrue(prepared.startswith(b"\xef\xbb\xbf"))
        self.assertTrue(lease["requested"])
        self.assertFalse(lease["installed"])
        lease = json.loads(json.dumps(lease))
        self.assertEqual(restore_effects_config(prepared, lease), (raw, ()))

    def test_new_section_no_final_newline_and_empty_file_roundtrip(self):
        for raw in (b"", b"\xef\xbb\xbf", b"; unchanged no newline", b"[OVERLAY]\nShowFPS=0"):
            with self.subTest(raw=raw):
                prepared, lease = prepare_effects_config(raw, PATH)
                self.assertIn(b"[GENERAL]", prepared)
                self.assertIn(b"NoReloadOnInit=0", prepared)
                self.assertIn(b"SkipLoadingDisabledEffects=0", prepared)
                self.assertEqual(restore_effects_config(prepared, lease), (raw, ()))

    def test_append_inside_general_preserves_other_section_and_mixed_endings(self):
        raw = b"[GENERAL]\r\nPresetPath=user.ini\n# comment\r\n[INPUT]\nKeyOverlay=36,0,0,0\n"
        prepared, lease = prepare_effects_config(raw, PATH)
        self.assertIn(b"PresetPath=user.ini\n# comment\r\n", prepared)
        self.assertIn(b"[INPUT]\nKeyOverlay=36,0,0,0\n", prepared)
        self.assertEqual(restore_effects_config(prepared, lease), (raw, ()))

    def test_modified_final_row_without_newline_restores_exact_original(self):
        for raw in (b"[GENERAL]\nNoReloadOnInit=1", b"[GENERAL]\nEffectSearchPaths=C:\\Other",
                    b"[GENERAL]\nSkipLoadingDisabledEffects=1"):
            with self.subTest(raw=raw):
                prepared, lease = prepare_effects_config(raw, PATH)
                self.assertEqual(restore_effects_config(prepared, lease), (raw, ()))

    def test_existing_owned_looking_path_is_not_claimed_or_removed(self):
        raw = b"[GENERAL]\nEffectSearchPaths=d:/texturequest/effects\nNoReloadOnInit=0\nSkipLoadingDisabledEffects=0\n"
        prepared, lease = prepare_effects_config(raw, PATH)
        self.assertEqual(prepared, raw)
        self.assertFalse(lease["path_added"])
        self.assertEqual(lease["changes"], [])
        self.assertEqual(restore_effects_config(prepared, lease), (raw, ()))

    def test_implicit_default_search_path_retained_but_explicit_empty_not_added(self):
        prepared, lease = prepare_effects_config(b"[GENERAL]\n", PATH)
        self.assertIn(b"EffectSearchPaths=.\\,D:\\TextureQuest\\effects\n", prepared)
        self.assertEqual(restore_effects_config(prepared, lease), (b"[GENERAL]\n", ()))
        raw = b"[GENERAL]\nEffectSearchPaths=\n"
        prepared, lease = prepare_effects_config(raw, PATH)
        self.assertIn(b"EffectSearchPaths=D:\\TextureQuest\\effects\n", prepared)
        self.assertNotIn(b"EffectSearchPaths=.\\,", prepared)
        self.assertEqual(restore_effects_config(prepared, lease), (raw, ()))

    def test_unicode_line_separator_in_path_is_not_an_ini_boundary(self):
        raw = "[GENERAL]\nEffectSearchPaths=D:\\Other\u2028Folder\nNoReloadOnInit=1\n".encode("utf-8")
        prepared, lease = prepare_effects_config(raw, PATH)
        self.assertIn("D:\\Other\u2028Folder,D:\\TextureQuest\\effects".encode("utf-8"), prepared)
        self.assertEqual(restore_effects_config(prepared, lease), (raw, ()))

    def test_unrelated_external_edits_survive_restore(self):
        raw = b"[GENERAL]\nEffectSearchPaths=C:\\Other\nNoReloadOnInit=1\nPresetPath=before.ini\n"
        prepared, lease = prepare_effects_config(raw, PATH)
        changed = prepared.replace(b"PresetPath=before.ini", b"PresetPath=after.ini") + b"[MY_ADDON]\nStrength=0.7\n"
        restored, conflicts = restore_effects_config(changed, lease)
        self.assertFalse(conflicts)
        self.assertEqual(restored, raw.replace(b"PresetPath=before.ini", b"PresetPath=after.ini") + b"[MY_ADDON]\nStrength=0.7\n")

    def test_changed_target_preserved_while_other_owned_values_restore(self):
        raw = b"[GENERAL]\nEffectSearchPaths=C:\\Other\nNoReloadOnInit=1\nSkipLoadingDisabledEffects=1\n"
        prepared, lease = prepare_effects_config(raw, PATH)
        changed = prepared.replace(b"EffectSearchPaths=C:\\Other,D:\\TextureQuest\\effects", b"EffectSearchPaths=C:\\NewUserChoice")
        restored, conflicts = restore_effects_config(changed, lease)
        self.assertEqual(conflicts, ("EffectSearchPaths",))
        self.assertIn(b"EffectSearchPaths=C:\\NewUserChoice", restored)
        self.assertIn(b"NoReloadOnInit=1", restored)
        self.assertIn(b"SkipLoadingDisabledEffects=1", restored)

    def test_restore_accepts_formatter_changes_when_owned_value_still_exact(self):
        raw = b"[GENERAL]\nNoReloadOnInit=1\nSkipLoadingDisabledEffects=1\n"
        prepared, lease = prepare_effects_config(raw, PATH)
        changed = prepared.replace(b"NoReloadOnInit=0\n", b" NoReloadOnInit = 0 \r\n")
        restored, conflicts = restore_effects_config(changed, lease)
        self.assertFalse(conflicts)
        self.assertIn(b" NoReloadOnInit = 1 \r\n", restored)

    def test_repeated_restore_does_not_reinsert_deleted_owned_rows(self):
        raw = b"[GENERAL]\nPresetPath=mine.ini\n"
        prepared, lease = prepare_effects_config(raw, PATH)
        restored, conflicts = restore_effects_config(prepared, lease)
        self.assertEqual((restored, conflicts), (raw, ()))
        self.assertEqual(restore_effects_config(restored, lease), (raw, ()))

    def test_duplicate_affected_keys_refuse_prepare_and_conflict_restore(self):
        for key in ("EffectSearchPaths", "NoReloadOnInit", "SkipLoadingDisabledEffects"):
            raw = ("[GENERAL]\n" + key + "=1\n" + key + "=2\n").encode()
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "Duplicate"):
                prepare_effects_config(raw, PATH)
        raw = b"[GENERAL]\nNoReloadOnInit=1\n"
        prepared, lease = prepare_effects_config(raw, PATH)
        changed = prepared + b"NoReloadOnInit=0\n"
        restored, conflicts = restore_effects_config(changed, lease)
        self.assertIn("NoReloadOnInit", conflicts)
        self.assertEqual(restored.count(b"NoReloadOnInit=0"), 2)

    def test_exact_case_sections_follow_reshade_not_configparser(self):
        raw = b"[general]\nEffectSearchPaths=ignored-case\nNoReloadOnInit=1\n"
        prepared, lease = prepare_effects_config(raw, PATH)
        self.assertTrue(prepared.startswith(raw))
        self.assertIn(b"[GENERAL]", prepared)
        self.assertEqual(restore_effects_config(prepared, lease), (raw, ()))

    def test_empty_search_entries_and_unsafe_paths_refuse(self):
        for value in (b"C:\\One,", b",C:\\One"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "Empty"):
                prepare_effects_config(b"[GENERAL]\nEffectSearchPaths=" + value + b"\n", PATH)
        for path in ("relative/effects", r"D:effects", "D:\\effects\n[INPUT]", r"\\server\share\effects", r"D:\effects\**"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                prepare_effects_config(b"", path)

    def test_invalid_bytes_and_journal_fail_before_restore(self):
        for raw in (b"\xff", b"[GENERAL]\x00", b"[GENERAL\nNoReloadOnInit=1\n", b"x" * (4 * 1024**2 + 1)):
            with self.subTest(size=len(raw)), self.assertRaises((ValueError, UnicodeDecodeError)):
                prepare_effects_config(raw, PATH)
        prepared, lease = prepare_effects_config(b"", PATH)
        altered = copy.deepcopy(lease)
        altered["changes"][0]["key"] = "PresetPath"
        with self.assertRaises(ValueError):
            restore_effects_config(prepared, altered)
        # Check semantic validation too, not merely the record checksum.
        body = {key: value for key, value in altered.items() if key != "record_sha256"}
        altered["record_sha256"] = hashlib.sha256(json.dumps(body, sort_keys=True, ensure_ascii=False,
                                                            separators=(",", ":")).encode()).hexdigest()
        with self.assertRaises(ValueError):
            restore_effects_config(prepared, altered)


if __name__ == "__main__":
    unittest.main()
