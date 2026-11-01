"""Synthetic byte-only hotkey policy tests; no game, ReShade or GPU calls."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from texturequest_hotkeys import sanitize_reshade_config, sanitize_reshade_preset


class ReShadeHotkeyTests(unittest.TestCase):
    def test_existing_keys_disabled_except_home_with_comments_preserved(self):
        before = (b"; custom\r\n[INPUT]\r\nKeyOverlay = 121,0,0,0 ; menu\r\n"
                  b"KeyReload=116,0,0,0\r\nKeyUnknownFuture=118,0,0,0\r\nInputProcessing=2\r\n"
                  b"[STYLE]\r\nFont=custom.ttf\r\n")
        after = sanitize_reshade_config(before)
        self.assertIn(b"KeyOverlay = 36,0,0,0 ; menu\r\n", after)
        self.assertIn(b"KeyReload=0,0,0,0\r\n", after)
        self.assertIn(b"KeyUnknownFuture=0,0,0,0\r\n", after)
        self.assertIn(b"InputProcessing=2\r\n", after)
        self.assertIn(b"[STYLE]\r\nFont=custom.ttf\r\n", after)
        self.assertEqual(sanitize_reshade_config(after), after)

    def test_missing_explicit_local_keys_block_global_fallback(self):
        after = sanitize_reshade_config(b"[GENERAL]\nPresetPath=mine.ini\n")
        for key in ("KeyEffects", "KeyScreenshot", "KeyNextPreset", "KeyPreviousPreset",
                    "KeyReload", "KeyFPS", "KeyFrameTime", "KeyPerformanceMode"):
            self.assertIn((key + "=0,0,0,0\n").encode(), after)
        self.assertIn(b"KeyOverlay=36,0,0,0\n", after)
        self.assertIn(b"PresetPath=mine.ini\n", after)

    def test_preset_lists_blank_but_path_and_effects_unchanged(self):
        after = sanitize_reshade_config(
            b"[GENERAL]\nPresetShortcutKeys = 51,0,0,1 ; Alt3\nPresetShortcutPaths=one.ini,two.ini\n"
            b"PresetPath=mine.ini\nEffectSearchPaths=.\\my-shaders\\**\n[ADDON]\nAddonPath=mine\n")
        self.assertIn(b"PresetShortcutKeys =  ; Alt3\n", after)
        self.assertIn(b"PresetShortcutPaths=\n", after)
        self.assertIn(b"PresetPath=mine.ini\nEffectSearchPaths=.\\my-shaders\\**\n[ADDON]\nAddonPath=mine\n", after)
        self.assertEqual(sanitize_reshade_config(after), after)

    def test_duplicates_case_and_no_final_newline_are_idempotent(self):
        before = b"[input]\nkeyoverlay=0,0,0,0\nKEYFPS=119,0,0,0\n[INPUT]\nKeyOverlay=121,0,0,0"
        after = sanitize_reshade_config(before)
        self.assertEqual(after.count(b"36,0,0,0"), 2)
        self.assertIn(b"KEYFPS=0,0,0,0", after)
        self.assertEqual(sanitize_reshade_config(after), after)

    def test_bom_unicode_and_unrelated_lines_preserved(self):
        before = b"\xef\xbb\xbf" + "[STYLE]\r\nFont=C:\\テスト\\font.ttf\r\n".encode()
        after = sanitize_reshade_config(before)
        self.assertTrue(after.startswith(before))
        self.assertEqual(after.count(b"\xef\xbb\xbf"), 1)
        self.assertEqual(sanitize_reshade_config(after), after)

    def test_all_builtin_keys_and_unknown_keys_not_addon_keys(self):
        before = b"[INPUT]\nKeyFPS=1,1,1,1\n[RENODX-DLSS]\nToggleKey=121\n[TextureQuest]\nShortcut=C:\\key.txt\n"
        after = sanitize_reshade_config(before)
        self.assertIn(b"[RENODX-DLSS]\nToggleKey=121\n[TextureQuest]\nShortcut=C:\\key.txt\n", after)
        self.assertIn(b"KeyFPS=0,0,0,0", after)

    def test_technique_and_uniform_preset_forms(self):
        before = (b"Techniques=AO@Mine.fx,Glow@Other.fx\nKeyAO@Mine.fx=121,0,0,0\n"
                  b"KeyGlow = 51,0,0,1 # Alt3\n[Mine.fx]\nKeyEnable=112,0,0,0\n"
                  b"Enable=1\nStrength=0.7\nKeyLight=1\nVector=1,0,0,0\n")
        after = sanitize_reshade_preset(before)
        self.assertIn(b"Techniques=AO@Mine.fx,Glow@Other.fx\n", after)
        self.assertIn(b"KeyAO@Mine.fx=0,0,0,0\n", after)
        self.assertIn(b"KeyGlow = 0,0,0,0 # Alt3\n", after)
        self.assertIn(b"KeyEnable=0,0,0,0\nEnable=1\nStrength=0.7\nKeyLight=1\nVector=1,0,0,0\n", after)
        self.assertEqual(sanitize_reshade_preset(after), after)

    def test_preset_unrelated_sections_and_nontuples_untouched(self):
        before = b"[ADDON]\nKeyTest=121,0,0,0\n[Mine.fx]\nKeyColor=1.0,0.2,0.3,1.0\nKeyValue=1,2,3\n"
        self.assertEqual(sanitize_reshade_preset(before), before)

    def test_preset_noncanonical_nonzero_modifiers_are_disabled(self):
        before = b"KeyAO@Mine.fx=121,2,3,4\n[Mine.fx]\nKeyEnabled=51,4294967295,0,0\n"
        self.assertEqual(sanitize_reshade_preset(before),
                         b"KeyAO@Mine.fx=0,0,0,0\n[Mine.fx]\nKeyEnabled=0,0,0,0\n")

    def test_preset_bom_crlf_and_empty_bytes(self):
        before = b"\xef\xbb\xbfKeyAO@Mine.fx=121,1,0,0\r\n[Mine.fx]\r\nValue=1"
        after = sanitize_reshade_preset(before)
        self.assertEqual(after, b"\xef\xbb\xbfKeyAO@Mine.fx=0,0,0,0\r\n[Mine.fx]\r\nValue=1")
        self.assertEqual(sanitize_reshade_preset(b""), b"")
        self.assertEqual(sanitize_reshade_preset(after), after)

    def test_invalid_bytes_size_and_section_refuse(self):
        for fn in (sanitize_reshade_config, sanitize_reshade_preset):
            with self.subTest(fn=fn.__name__):
                for raw in (b"\xff", b"x\x00y", b"[broken", b"x" * (4 * 1024**2 + 1)):
                    with self.assertRaises((ValueError, UnicodeError)):
                        fn(raw)
                with self.assertRaises(TypeError):
                    fn("text")
        with self.assertRaisesRegex(ValueError, "Sanitized"):
            sanitize_reshade_config(b";" + b"x" * (4 * 1024**2 - 1))


if __name__ == "__main__":
    unittest.main()
