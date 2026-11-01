"""Current AO/contact-only migration; D-only invented games, no renderer calls."""
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


class RemovedEffectBytes(unittest.TestCase):
    def test_exact_bounce_and_edge_selectors_retire_with_foreign_lookalikes_kept(self):
        raw = (b"Techniques=TQIndirectLighting@TQEffects.fx,AA@User.fx,TQAntiAliasing@TQEffects.fx,TQIndirectLighting@User.fx\r\n"
               b"TechniqueSorting=TQAmbientOcclusion@TQEffects.fx,TQIndirectLighting@TQEffects.fx,TQAntiAliasing@TQEffects.fx\r\n"
               b"KeyTQAntiAliasing@TQEffects.fx=121,0,0,0\r\nKeyTQIndirectLighting@TQEffects.fx=122,0,0,0\r\n"
               b"KeyAA@User.fx=51,0,0,0\r\n")
        expected = (b"Techniques=AA@User.fx,TQIndirectLighting@User.fx\r\n"
                    b"TechniqueSorting=TQAmbientOcclusion@TQEffects.fx\r\nKeyAA@User.fx=51,0,0,0\r\n")
        self.assertEqual(effects.retire_removed_effect_settings(raw), expected)

    def test_only_owned_removed_uniforms_retire_preserving_ao_contact_and_foreign(self):
        raw = (b"[TQEffects.fx]\nTQAAStrength=0.8\nTQAAQuality=1\nTQIndirectStrength=1\nTQIndirectRadius=3\n"
               b"TQIndirectEmissiveBoost=2\nTQIndirectThreshold=0.2\nTQIndirectQuality=2\n"
               b"TQAOStrength=1\nTQContactStrength=0.4\n[User.fx]\nTQAAStrength=keep\nTQIndirectStrength=keep\n")
        expected = b"[TQEffects.fx]\nTQAOStrength=1\nTQContactStrength=0.4\n[User.fx]\nTQAAStrength=keep\nTQIndirectStrength=keep\n"
        self.assertEqual(effects.retire_removed_effect_settings(raw), expected)

    def test_unicode_bom_mixed_endings_and_idempotence_for_all_removed_effects(self):
        raw = ("\ufeff; Café\r\n[GENERAL]\nTechniques = TQBloom@TQEffects.fx,TQAntiAliasing@TQEffects.fx,TQIndirectLighting@TQEffects.fx,User@Comma,,Name.fx \r\n"
               "[TQEffects.fx]\r\nTQGlowStrength=0.9\nTQAAThreshold=0.1\r\nTQIndirectStrength=1\nTQAOBias=0.04").encode()
        expected = ("\ufeff; Café\r\n[GENERAL]\nTechniques = User@Comma,,Name.fx \r\n[TQEffects.fx]\r\nTQAOBias=0.04").encode()
        result = effects.retire_removed_effect_settings(raw)
        self.assertEqual(result, expected)
        self.assertEqual(effects.retire_removed_effect_settings(result), result)

    def test_historical_bloom_only_api_keeps_its_narrow_contract(self):
        raw = b"Techniques=TQBloom@TQEffects.fx,TQAntiAliasing@TQEffects.fx,TQIndirectLighting@TQEffects.fx\n"
        self.assertEqual(effects.retire_bloom_settings(raw),
                         b"Techniques=TQAntiAliasing@TQEffects.fx,TQIndirectLighting@TQEffects.fx\n")


class RemovedEffectsNormalFlow(Fixture):
    def setUp(self):
        root = Path(os.environ["TQ_REMOVED_EFFECTS_TEST_ROOT"]).absolute()
        if root.drive.casefold() != "d:" or not root.is_relative_to(Path("D:/eq2 paks")):
            raise RuntimeError("Require a new owned D effects-removal scope")
        root.mkdir(parents=True, exist_ok=True)
        local = patch.object(tempfile, "tempdir", str(root)); local.start(); self.addCleanup(local.stop)
        super().setUp(); enable_fixture(self.runtime)
        self.selected = normalize(); self.selected["effects"] = True
        (self.game / "dxgi.dll").write_bytes(ADDON_BUILD)

    def test_normal_install_and_uninstall_retire_old_enabled_preferences_permanently(self):
        ini = (b"[GENERAL]\nEffectSearchPaths=.\\User\\**\nPresetPath=mine.ini\nStartupPresetPath=other.ini\n"
               b"Techniques=TQIndirectLighting@TQEffects.fx,TQAntiAliasing@TQEffects.fx,User@User.fx\n")
        (self.game / "ReShade.ini").write_bytes(ini)
        preset = (b"Techniques=TQAmbientOcclusion@TQEffects.fx,TQIndirectLighting@TQEffects.fx,TQAntiAliasing@TQEffects.fx,User@User.fx\n"
                  b"[TQEffects.fx]\nTQIndirectStrength=1\nTQAAStrength=1\nTQAOStrength=0.5\n[User.fx]\nStrength=0.4\n")
        for name in ("mine.ini", "other.ini"): (self.game / name).write_bytes(preset)
        (self.game / "paks/untouched.vpk").write_bytes(b"invented untouched game archive")
        self.install(components=self.selected)
        record = self.manager._record(self.game)
        self.assertEqual(record["effects_settings"]["journal"]["schema"], effects.SCHEMA_V3)
        self.assertEqual(record["components"], self.selected)
        for name in ("ReShade.ini", "mine.ini", "other.ini"):
            after = (self.game / name).read_bytes()
            self.assertNotIn(b"TQIndirectLighting@TQEffects.fx", after)
            self.assertNotIn(b"TQAntiAliasing@TQEffects.fx", after)
            self.assertIn(b"User@User.fx", after)
        self.assertIn(b"TQAOStrength=0.5", (self.game / "mine.ini").read_bytes())
        self.manager.uninstall(self.game)
        self.assertNotIn(b"TQIndirectLighting@TQEffects.fx", (self.game / "mine.ini").read_bytes())
        self.assertIn(b"Strength=0.4", (self.game / "other.ini").read_bytes())
        self.assertEqual((self.game / "paks/untouched.vpk").read_bytes(), b"invented untouched game archive")
        self.assertEqual((self.game / "dxgi.dll").read_bytes(), ADDON_BUILD)
        self.assertTrue(list((self.state / "reshade-settings-history").glob("*.ini")))

    def test_five_technique_bundle_is_readable_but_not_selectable(self):
        path = self.runtime / "RUNTIME.json"; doc = json.loads(path.read_bytes())
        doc["effects"] = dict(schema="texturequest-effects-runtime-v3", shader="effects/TQEffects.fx",
            effect_name="TQEffects.fx", techniques=["TQAmbientOcclusion", "TQContactShadows", "TQIndirectLighting", "TQAntiAliasing", "TQDepthDiagnostic"])
        path.write_text(json.dumps(doc))
        self.assertEqual(self.manager._runtime()["effects"]["schema"], "texturequest-effects-runtime-v3")
        with self.assertRaisesRegex(ValueError, "outdated"):
            self.manager.preflight_components(self.game, self.selected)
        self.assertFalse((self.game / "TextureQuest.addon64").exists())

    def test_malformed_removed_effect_preset_fails_preflight_without_mutation(self):
        raw = b"[GENERAL]\nPresetPath=mine.ini\nEffectSearchPaths=.\\User\n"
        (self.game / "ReShade.ini").write_bytes(raw)
        (self.game / "mine.ini").write_bytes(b"[TQEffects.fx]\n[broken\nTQIndirectStrength=1\n")
        with self.assertRaisesRegex(ValueError, "Malformed"):
            self.manager.preflight_components(self.game, self.selected)
        self.assertEqual((self.game / "ReShade.ini").read_bytes(), raw)
        self.assertFalse((self.game / "TextureQuest.addon64").exists())
