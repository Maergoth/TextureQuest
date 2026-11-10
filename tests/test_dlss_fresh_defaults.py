"""Current AIO defaults and old lease compatibility; pure bytes, no runtime."""
import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import texturequest_dlss as settings


class FreshDLSSDefaults(unittest.TestCase):
    def test_fresh_current_profile_starts_all_three_features_off_and_restores_exactly(self):
        raw = b'\xef\xbb\xbf; user note\r\n[USER]\r\nKeep=unchanged'
        after, lease = settings.prepare_dlss_config(raw, profile='aio2')
        self.assertEqual(lease['schema'], settings.AIO_V3_SCHEMA)
        for key in ('NeuralRendering', 'FrameGeneration', 'ManagedSrEnabled'):
            self.assertIn((key + '=0\r\n').encode(), after)
        self.assertIn(b'Enabled=1\r\n', after)
        self.assertEqual(len(settings.AIO2_VALUES), 19)
        self.assertEqual(settings.restore_dlss_config(after, lease), (raw, ()))

    def test_explicit_on_off_values_and_current_user_combination_survive_refresh(self):
        for nr, fg, sr in ((1, 1, 0), (0, 0, 1), (1, 0, 1), (0, 1, 0)):
            with self.subTest(nr=nr, fg=fg, sr=sr):
                raw = ('[Standalone.DLSSNR]\r\n'
                       f'NeuralRendering = {nr} ; keep NR\r\n'
                       f'FrameGeneration={fg}\r\nManagedSrEnabled = {sr} ; keep SR\r\n'
                       'SourceResolutionOverride=2\r\nManagedSrMode=4\r\nManagedFrameMultiplier=3\r\n').encode()
                after, lease = settings.prepare_dlss_config(raw, profile='aio2')
                for line in raw.splitlines(keepends=True):
                    self.assertIn(line, after)
                owned = {row['key'] for row in lease['changes']}
                self.assertFalse(owned & {'NeuralRendering', 'FrameGeneration', 'ManagedSrEnabled',
                                          'SourceResolutionOverride'})
                again, repeated = settings.prepare_dlss_config(after, profile='aio2')
                self.assertEqual(again, after); self.assertEqual(repeated['changes'], [])
                self.assertEqual(settings.restore_dlss_config(after, lease), (raw, ()))

    def test_partial_controls_only_fill_missing_defaults(self):
        raw = b'[Standalone.DLSSNR]\nNeuralRendering=1\nManagedSrEnabled=0\nUser=keep\n'
        after, lease = settings.prepare_dlss_config(raw, profile='aio2')
        self.assertIn(b'NeuralRendering=1\n', after)
        self.assertIn(b'ManagedSrEnabled=0\n', after)
        self.assertIn(b'FrameGeneration=0\n', after)
        owned = {row['key'] for row in lease['changes']}
        self.assertIn('FrameGeneration', owned)
        self.assertNotIn('NeuralRendering', owned); self.assertNotIn('ManagedSrEnabled', owned)
        self.assertEqual(settings.restore_dlss_config(after, lease), (raw, ()))

    def test_later_user_enabling_new_defaults_survives_uninstall(self):
        after, lease = settings.prepare_dlss_config(b'', profile='aio2')
        edited = after
        for key in ('NeuralRendering', 'FrameGeneration', 'ManagedSrEnabled'):
            edited = edited.replace((key + '=0').encode(), (key + '=1').encode())
        restored, conflicts = settings.restore_dlss_config(edited, lease)
        for key in ('NeuralRendering', 'FrameGeneration', 'ManagedSrEnabled'):
            self.assertIn((key + '=1').encode(), restored)
            self.assertIn('Standalone.DLSSNR.' + key, conflicts)
        self.assertNotIn(b'WindowedInputScaling=', restored)

    def test_genuine_old_v2_journal_and_legacy_profiles_keep_original_defaults(self):
        fixture = json.loads((Path(__file__).parent / 'fixtures/dlss-aio-v2-lease.json').read_text())
        raw, after = bytes.fromhex(fixture['raw_hex']), bytes.fromhex(fixture['after_hex'])
        self.assertEqual(fixture['journal']['schema'], settings.AIO_V2_SCHEMA)
        self.assertIn(b'NeuralRendering=1', after); self.assertIn(b'FrameGeneration=1', after)
        self.assertNotIn(b'ManagedSrEnabled=', after)
        self.assertEqual(settings.restore_dlss_config(after, fixture['journal']), (raw, ()))
        legacy, lease = settings.prepare_dlss_config(raw, profile='aio')
        self.assertEqual(lease['schema'], settings.AIO_SCHEMA)
        self.assertIn(b'NeuralRendering=1', legacy); self.assertIn(b'FrameGeneration=1', legacy)
        self.assertNotIn(b'ManagedSrEnabled=', legacy)
        self.assertEqual(settings.restore_dlss_config(legacy, lease), (raw, ()))
        feeder, lease = settings.prepare_dlss_config(raw, profile='feeder')
        self.assertEqual(lease['schema'], settings.SCHEMA)
        self.assertIn(b'NeuralUplift=1', feeder)
        self.assertEqual(settings.restore_dlss_config(feeder, lease), (raw, ()))

    def test_new_sr_duplicates_and_wrong_journal_authority_refuse(self):
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            settings.prepare_dlss_config(b'[Standalone.DLSSNR]\nManagedSrEnabled=0\nManagedSrEnabled=1\n', profile='aio2')
        after, lease = settings.prepare_dlss_config(b'', profile='aio2')
        forged = copy.deepcopy(lease)
        row = next(row for row in forged['changes'] if row['key'] == 'ManagedSrEnabled')
        row['after'] = row['after'].replace('=0', '=1')
        forged = settings._seal({k: v for k, v in forged.items() if k != 'record_sha256'})
        with self.assertRaisesRegex(ValueError, 'after-value is not owned'):
            settings.restore_dlss_config(after, forged)
        old_schema = settings._seal(dict({k: v for k, v in lease.items() if k != 'record_sha256'},
            schema=settings.AIO_V2_SCHEMA,
            changes=[row for row in lease['changes'] if row['key'] == 'ManagedSrEnabled']))
        with self.assertRaisesRegex(ValueError, 'Unknown DLSS changed key'):
            settings.restore_dlss_config(after, old_schema)


class PublicGraphicsDefaults(unittest.TestCase):
    def test_fresh_public_defaults_and_conditional_restore(self):
        raw = b'; original\r\n[Foreign]\r\nKeep=1\r\n'
        after, lease = settings.prepare_dlss_config(raw, profile='graphics-world-aio-defaults')
        self.assertTrue(lease['schema'].endswith('-v4'))
        for key in ('FrameGeneration', 'ManagedSrEnabled', 'NeuralRendering',
                    'SmoothInterfaceText', 'SunLighting'):
            self.assertIn((key + '=0\r\n').encode(), after)
        self.assertIn(b'AmbientOcclusion=1\r\n', after)
        self.assertNotIn(b'ReflexLowLatency', after)
        self.assertIn(b'ShadowedLights=64\r\n', after)
        self.assertIn(b'ParticleShadowLimit=8\r\n', after)
        self.assertIn(b'EmissiveLightLimit=32\r\n', after)
        self.assertIn(b'ParticleLightLimit=16\r\n', after)
        self.assertEqual(settings.restore_dlss_config(after, lease), (raw, ()))
        edited = after.replace(b'PointLightShadows=1', b'PointLightShadows=0')
        restored, conflicts = settings.restore_dlss_config(edited, lease)
        self.assertIn(b'PointLightShadows=0', restored)
        self.assertIn('EQ2Graphics.PointLightShadows', conflicts)

    def test_saved_choices_are_not_reset_by_new_defaults(self):
        raw = (b'[EQ2Graphics]\r\nPointLightShadows=1\r\nEmissiveLights=1\r\n'
               b'AmbientOcclusion=0\r\nSunLighting=1\r\nSunSoftness=2.5\r\n'
               b'SmoothInterfaceText=1\r\nShadowedLights=27\r\nParticleLightLimit=37\r\n'
               b'[Standalone.DLSSNR]\r\nFrameGeneration=1\r\nNeuralRendering=1\r\n'
               b'[TextureQuest]\r\nReflexLowLatency=1\r\n')
        after, lease = settings.prepare_dlss_config(raw, profile='graphics-world-aio-defaults')
        for line in raw.splitlines(keepends=True):
            self.assertIn(line, after)
        self.assertEqual(settings.restore_dlss_config(after, lease), (raw, ()))
        repeated, journal = settings.prepare_dlss_config(after, profile='graphics-world-aio-defaults')
        self.assertEqual(repeated, after)
        self.assertEqual(journal['changes'], [])

    def test_genuine_v1_defaults_lease_still_restores_exactly(self):
        fixture = json.loads((Path(__file__).parent / 'fixtures/graphics-defaults-v1-lease.json').read_text())
        raw, after = bytes.fromhex(fixture['raw_hex']), bytes.fromhex(fixture['after_hex'])
        self.assertTrue(fixture['journal']['schema'].endswith('-v1'))
        self.assertIn(b'ReflexLowLatency=1', after)
        self.assertIn(b'SunLighting=1', after)
        self.assertEqual(settings.restore_dlss_config(after, fixture['journal']), (raw, ()))


if __name__ == '__main__':
    unittest.main()
