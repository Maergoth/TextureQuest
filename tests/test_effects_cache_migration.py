"""Exact historical private-FX search-path migration; invented D-only games."""
import hashlib
import os
from pathlib import Path
import tempfile
from unittest.mock import patch

from test_graphics_proxy import Fixture, ADDON_BUILD
from test_effects_install import enable_fixture
import texturequest_effects as effects
from texturequest_components import normalize

ROOT = Path(os.environ["TQ_EFFECTS_MIGRATION_ROOT"]).absolute()
if ROOT.drive.casefold() != "d:" or not ROOT.is_relative_to(Path("D:/eq2 paks")):
    raise RuntimeError("Require owned D effects migration test root")
ROOT.mkdir(parents=True, exist_ok=True)
LEGACY_SOURCE = Path(r"D:/eq2 paks/work/texturequest_graphics_features_20261031/package-002/build/graphics-runtime/effects/TQEffects.fx")


class LegacyEffectsMigration(Fixture):
    def setUp(self):
        local = patch.object(tempfile, "tempdir", str(ROOT)); local.start(); self.addCleanup(local.stop)
        super().setUp()
        self.assertTrue(self.root.resolve().is_relative_to(ROOT.resolve()))
        enable_fixture(self.runtime)
        self.selected = normalize(); self.selected["effects"] = True
        self.legacy = self.state / "effects" / effects.LEGACY_SHADER_SHA256
        self.legacy.mkdir(parents=True)
        raw = LEGACY_SOURCE.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), effects.LEGACY_SHADER_SHA256)
        (self.legacy / "TQEffects.fx").write_bytes(raw)
        (self.game / "dxgi.dll").write_bytes(ADDON_BUILD)

    def search(self):
        raw = (self.game / "ReShade.ini").read_bytes()
        return next(line for line in raw.decode().splitlines() if line.startswith("EffectSearchPaths="))

    def test_exact_legacy_cache_migrates_normally_and_uninstall_restores_old_value(self):
        old = ".\\user\\**," + str(self.legacy).replace("\\", "/")
        (self.game / "ReShade.ini").write_bytes(("[GENERAL]\nEffectSearchPaths=" + old + "\nNoReloadOnInit=1\n").encode())
        before = (self.legacy / "TQEffects.fx").read_bytes()
        self.install(components=self.selected)
        journal = self.manager._record(self.game)["effects_settings"]["journal"]
        self.assertEqual(journal["schema"], effects.SCHEMA_V2)
        self.assertEqual(journal["retired_shader_directory"], str(self.legacy))
        self.assertNotIn(effects.LEGACY_SHADER_SHA256, self.search())
        self.assertIn(".\\user\\**", self.search())
        self.assertEqual((self.legacy / "TQEffects.fx").read_bytes(), before)
        result = self.manager.uninstall(self.game)
        self.assertEqual(result["settings_conflicts"], ())
        self.assertEqual(self.search(), "EffectSearchPaths=" + old)
        self.assertEqual((self.legacy / "TQEffects.fx").read_bytes(), before)

    def test_hash_name_alone_or_extra_user_file_is_not_claimed(self):
        runtime = self.manager._runtime()
        (self.legacy / "TQEffects.fx").write_bytes(b"unrelated edited shader")
        _, retired = self.manager._effects_shader(runtime)
        self.assertIsNone(retired)
        (self.legacy / "TQEffects.fx").write_bytes(LEGACY_SOURCE.read_bytes())
        (self.legacy / "user.fx").write_bytes(b"unrelated user code")
        _, retired = self.manager._effects_shader(runtime)
        self.assertIsNone(retired)
        self.assertEqual((self.legacy / "user.fx").read_bytes(), b"unrelated user code")

    def test_only_exact_owned_legacy_directory_entry_is_removed(self):
        other = str(self.root / "user/effects" / effects.LEGACY_SHADER_SHA256)
        lookalike = str(self.state / "effects" / ("f" * 64))
        old = other + "," + str(self.legacy) + "," + lookalike
        (self.game / "ReShade.ini").write_bytes(("[GENERAL]\nEffectSearchPaths=" + old + "\n").encode())
        self.install(components=self.selected)
        paths = effects._split_paths(self.search().split("=", 1)[1])
        self.assertIn(other, paths); self.assertIn(lookalike, paths)
        self.assertNotIn(str(self.legacy), paths)
        self.assertEqual(self.manager.uninstall(self.game)["settings_conflicts"], ())
        self.assertEqual(self.search(), "EffectSearchPaths=" + old)

    def test_v1_restore_remains_supported_and_v2_cannot_retire_arbitrary_path(self):
        raw = b"[GENERAL]\nEffectSearchPaths=D:\\User\nNoReloadOnInit=1\n"
        prepared, journal = effects.prepare_effects_config(raw, r"D:\TextureQuest\effects\new")
        body = {k: v for k, v in journal.items() if k not in ("record_sha256", "retired_shader_directory")}
        body["schema"] = effects.SCHEMA
        self.assertEqual(effects.restore_effects_config(prepared, effects._seal(body)), (raw, ()))
        with self.assertRaisesRegex(ValueError, "verified legacy"):
            effects.prepare_effects_config(raw, "D:/TextureQuest/effects/" + "a" * 64,
                retired_shader_directory="D:/User/effects/" + effects.LEGACY_SHADER_SHA256)
