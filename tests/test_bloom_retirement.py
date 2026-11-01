"""Removed owned bloom migration; invented D-only installs, no graphics calls."""
from pathlib import Path
import json
import os
import tempfile
import unittest
from unittest.mock import patch

from test_graphics_proxy import Fixture, ADDON_BUILD
from test_effects_install import enable_fixture
import texturequest_effects as effects
from texturequest_components import normalize


class BloomBytes(unittest.TestCase):
    def test_only_exact_owned_selector_and_uniforms_retire(self):
        raw = ("\ufeff; Café settings\r\n"
               "Techniques = TQBloom@TQEffects.fx, Bloom@Bloom.fx,Other@Comma,,Name.fx \r\n"
               "TechniqueSorting=TQAmbientOcclusion@TQEffects.fx,TQBloom@TQEffects.fx\n"
               "KeyTQBloom@TQEffects.fx=121,0,0,0\r\n"
               "KeyBloom@Bloom.fx=51,0,1,0\r\n"
               "[TQEffects.fx]\r\nTQGlowStrength=0.8\r\nTQGlowThreshold=0.1\r\n"
               "TQGlowRadius=12\r\nTQBloomQuality=1\r\nTQAAStrength=0.65\r\n"
               "[Bloom.fx]\r\nTQGlowStrength=keep\r\nStrength=0.4").encode()
        result = effects.retire_bloom_settings(raw)
        expected = ("\ufeff; Café settings\r\n"
                    "Techniques =  Bloom@Bloom.fx,Other@Comma,,Name.fx \r\n"
                    "TechniqueSorting=TQAmbientOcclusion@TQEffects.fx\n"
                    "KeyBloom@Bloom.fx=51,0,1,0\r\n"
                    "[TQEffects.fx]\r\nTQAAStrength=0.65\r\n"
                    "[Bloom.fx]\r\nTQGlowStrength=keep\r\nStrength=0.4").encode()
        self.assertEqual(result, expected)
        self.assertEqual(effects.retire_bloom_settings(result), result)

    def test_foreign_same_name_and_commented_or_other_section_fields_are_unchanged(self):
        raw = (b";Techniques=TQBloom@TQEffects.fx\n"
               b"Techniques=TQBloom@Foreign.fx,Bloom@Bloom.fx\n"
               b"[Other.fx]\nTechniques=TQBloom@TQEffects.fx\nTQBloomQuality=0\n")
        self.assertEqual(effects.retire_bloom_settings(raw), raw)

    def test_duplicate_owned_entries_clear_but_unknown_section_refuses(self):
        raw = b"Techniques=TQBloom@TQEffects.fx,TQBloom@TQEffects.fx\r\nTechniques=User@User.fx\r\n"
        self.assertEqual(effects.retire_bloom_settings(raw), b"Techniques=\r\nTechniques=User@User.fx\r\n")
        with self.assertRaisesRegex(ValueError, "Malformed"):
            effects.retire_bloom_settings(b"[TQEffects.fx]\n[broken\nTQGlowStrength=0.2\n")

    def test_new_lease_identity_and_both_legacy_leases_restore(self):
        raw = b"[GENERAL]\nNoReloadOnInit=1\nEffectSearchPaths=D:\\User\n"
        prepared, lease = effects.prepare_effects_config(raw, r"D:\TextureQuest\effects\new")
        self.assertEqual((lease["schema"], lease["technique"]),
                         (effects.SCHEMA_V3, "TQAmbientOcclusion"))
        self.assertEqual(effects.restore_effects_config(prepared, lease), (raw, ()))
        for schema in (effects.SCHEMA, effects.SCHEMA_V2):
            body = {k: v for k, v in lease.items() if k != "record_sha256"}
            body.update(schema=schema, technique="TQBloom")
            if schema == effects.SCHEMA: body.pop("retired_shader_directory")
            self.assertEqual(effects.restore_effects_config(prepared, effects._seal(body)), (raw, ()))


class BloomNormalFlow(Fixture):
    def setUp(self):
        root = Path(os.environ["TQ_BLOOM_TEST_ROOT"]).absolute()
        if root.drive.casefold() != "d:" or not root.is_relative_to(Path("D:/eq2 paks")):
            raise RuntimeError("Require a new owned D bloom-test scope")
        root.mkdir(parents=True, exist_ok=True)
        local = patch.object(tempfile, "tempdir", str(root)); local.start(); self.addCleanup(local.stop)
        super().setUp()
        enable_fixture(self.runtime)
        self.selected = normalize(); self.selected["effects"] = True
        (self.game / "dxgi.dll").write_bytes(ADDON_BUILD)

    def test_normal_install_migrates_all_configured_presets_and_uninstall_does_not_reenable_bloom(self):
        ini = (b"[GENERAL]\nEffectSearchPaths=.\\User\\**\nPresetPath=mine.ini\n"
               b"StartupPresetPath=other.ini\nTechniques=TQBloom@TQEffects.fx,Bloom@Bloom.fx\n"
               b"[TQEffects.fx]\nTQGlowStrength=0.8\nTQAAStrength=0.65\n")
        (self.game / "ReShade.ini").write_bytes(ini)
        preset = b"Techniques=TQBloom@TQEffects.fx,Bloom@Bloom.fx\n[Bloom.fx]\nStrength=0.4\n"
        for name in ("mine.ini", "other.ini"): (self.game / name).write_bytes(preset)
        (self.game / "paks/untouched.vpk").write_bytes(b"invented untouched game archive")
        self.install(components=self.selected)
        self.assertEqual(self.manager._record(self.game)["effects_settings"]["journal"]["schema"], effects.SCHEMA_V3)
        for name in ("ReShade.ini", "mine.ini", "other.ini"):
            after = (self.game / name).read_bytes()
            self.assertNotIn(b"TQBloom@TQEffects.fx", after)
            self.assertNotIn(b"TQGlowStrength", after)
            self.assertIn(b"Bloom@Bloom.fx", after)
        self.assertIn(b".\\User\\**", (self.game / "ReShade.ini").read_bytes())
        self.manager.uninstall(self.game)
        self.assertNotIn(b"TQBloom@TQEffects.fx", (self.game / "mine.ini").read_bytes())
        self.assertIn(b"Strength=0.4", (self.game / "other.ini").read_bytes())
        self.assertEqual((self.game / "paks/untouched.vpk").read_bytes(), b"invented untouched game archive")
        self.assertEqual((self.game / "dxgi.dll").read_bytes(), ADDON_BUILD)
        self.assertTrue(list((self.state / "reshade-settings-history").glob("*.ini")))

    def test_old_bloom_bundle_cannot_be_selected_for_new_install(self):
        path = self.runtime / "RUNTIME.json"; doc = json.loads(path.read_bytes())
        doc["effects"]["schema"] = "texturequest-effects-runtime-v2"
        doc["effects"]["techniques"].insert(3, "TQBloom")
        path.write_text(json.dumps(doc))
        with self.assertRaisesRegex(ValueError, "outdated"):
            self.manager.preflight_components(self.game, self.selected)
        self.assertFalse((self.game / "TextureQuest.addon64").exists())
        self.assertFalse((self.game / "ReShade.ini").exists())

    def test_malformed_preset_is_detected_during_preflight_before_mutation(self):
        raw = b"[GENERAL]\nPresetPath=mine.ini\nEffectSearchPaths=.\\User\n"
        (self.game / "ReShade.ini").write_bytes(raw)
        (self.game / "mine.ini").write_bytes(b"[TQEffects.fx]\n[broken\nTQGlowStrength=0.4\n")
        with self.assertRaisesRegex(ValueError, "Malformed"):
            self.manager.preflight_components(self.game, self.selected)
        self.assertEqual((self.game / "ReShade.ini").read_bytes(), raw)
        self.assertFalse((self.game / "TextureQuest.addon64").exists())
