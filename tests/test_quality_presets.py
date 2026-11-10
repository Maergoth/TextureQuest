"""Real settings transactions against invented game folders; no GPU or live game."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_graphics_proxy import Fixture
import test_world_renderer_install as world_fixture
import texturequest_quality as quality
import texturequest_components as components
import texturequest_dlss as dlss

TEST_ROOT = Path(__file__).resolve().parents[2] / 'quality-validation' / 'fake-layouts'


class QualitySettings(unittest.TestCase):
    def test_presets_roundtrip_and_manual_divergence_preserve_unrelated_controls(self):
        original = (b'\xef\xbb\xbf[User]\r\nKeep=unchanged\r\n[Standalone.DLSSNR]\r\n'
                    b'FrameGeneration=1\r\nNeuralRendering=1\r\n[TextureQuest]\r\nShowFPS=0\r\n'
                    b'[eq2graphics]\r\nAnisotropy = 4 ; keep this comment\r\n'
                    b'ParticleLightStrengthV3 = 1.25 ; keep brightness\r\nParticleLightStrengthV2=0.5\r\n')
        for name in quality.NAMES:
            with self.subTest(name=name):
                updated = quality.apply(original, name)
                self.assertEqual(quality.detect(updated), name)
                self.assertEqual(quality.apply(updated, name), updated)
                self.assertIn(b'Anisotropy = 16 ; keep this comment', updated)
                for kept in (b'Keep=unchanged', b'FrameGeneration=1', b'NeuralRendering=1', b'ShowFPS=0',
                             b'ParticleLightStrengthV3 = 1.25 ; keep brightness', b'ParticleLightStrengthV2=0.5'):
                    self.assertIn(kept, updated)
                changed = updated.replace(b'Anisotropy = 16', b'Anisotropy = 8')
                self.assertEqual(quality.detect(changed), quality.CUSTOM)
        self.assertEqual(quality.detect(b''), 'Low')

    def test_duplicate_casefolded_settings_refuse_instead_of_guessing(self):
        bad = b'[EQ2Graphics]\nAnisotropy=16\n[eq2graphics]\nanisotropy=4\n'
        for action in (lambda: quality.apply(bad, 'Max'), lambda: quality.detect(bad)):
            with self.assertRaisesRegex(ValueError, 'Duplicate'):
                action()

    def test_fresh_defaults_match_low_old_v2_leases_still_restore_and_custom_stays(self):
        fresh, lease = dlss.prepare_dlss_config(b'', profile='graphics-world-defaults')
        self.assertEqual(quality.detect(fresh), 'Low')
        self.assertTrue(lease['schema'].endswith('-v5'))
        custom = quality.apply(b'', 'Max').replace(b'Anisotropy=16', b'Anisotropy=4')
        refreshed, _ = dlss.prepare_dlss_config(custom, profile='graphics-world-defaults')
        self.assertIn(b'Anisotropy=4', refreshed)
        self.assertEqual(quality.detect(refreshed), quality.CUSTOM)
        with patch.dict(dlss.GRAPHICS_DEFAULT_PROFILES,
                        {'graphics-world-defaults': dlss.V2_GRAPHICS_DEFAULT_PROFILES['graphics-world-defaults']}):
            old, old_lease = dlss.prepare_dlss_config(b'', profile='graphics-world-defaults')
        restored, conflicts = dlss.restore_dlss_config(old, old_lease)
        self.assertEqual(restored, b'')
        self.assertFalse(conflicts)

    def test_pending_choice_is_per_game_cas_and_cancelable_without_game_write(self):
        TEST_ROOT.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as directory:
            root = Path(directory)
            game, other, state = root / 'game', root / 'other', root / 'data'
            before = quality.load_pending(state, game)
            selected = quality.save_pending(state, game, 'High', expected=before)
            self.assertEqual(quality.load_pending(state, game), selected)
            self.assertIsNone(quality.load_pending(state, other)['choice'])
            self.assertFalse(game.exists())
            with self.assertRaisesRegex(ValueError, 'another window'):
                quality.save_pending(state, game, 'Max', expected=before)
            quality.save_pending(state, game, None, expected=selected)
            self.assertIsNone(quality.load_pending(state, game)['choice'])


class QualityInstall(Fixture):
    renderer_runtime = world_fixture.WorldRendererInstall.renderer_runtime

    def setUp(self):
        TEST_ROOT.mkdir(parents=True, exist_ok=True)
        temporary = patch.object(tempfile, 'tempdir', str(TEST_ROOT))
        temporary.start(); self.addCleanup(temporary.stop)
        super().setUp()
        self.renderer_runtime()
        closed = patch('texturequest_game_settings.ensure_closed')
        closed.start(); self.addCleanup(closed.stop)
        self.install(components=dict(components.normalize(), effects=True))

    def test_settings_only_apply_keeps_files_native_settings_and_does_no_texture_work(self):
        ini = self.game / 'ReShade.ini'
        native = self.game / 'eq2_recent.ini'
        native.write_bytes(b'unrelated native settings')
        before_record = self.manager._record(self.game)
        self.manager._activate_prepared.reset_mock()
        result = self.manager.apply_quality(self.game, 'Max')
        self.assertEqual(quality.detect(ini.read_bytes()), 'Max')
        self.assertIn('Max quality saved', result['message'])
        self.assertEqual(native.read_bytes(), b'unrelated native settings')
        self.assertEqual(before_record, self.manager._record(self.game))
        self.manager._activate_prepared.assert_not_called()
        self.assertTrue(Path(result['receipt']).is_file())

    def test_open_game_refuses_without_settings_write(self):
        ini = self.game / 'ReShade.ini'
        before = ini.read_bytes()
        with patch('texturequest_graphics_proxy.ensure_closed', side_effect=ValueError('Close EQ2')):
            with self.assertRaisesRegex(ValueError, 'Close EQ2'):
                self.manager.apply_quality(self.game, 'High')
        self.assertEqual(ini.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
