"""Normal refresh must not seed away the combined renderer's legacy filter."""
import unittest
from texturequest_dlss import prepare_dlss_config, restore_dlss_config


class LegacyPointSoftness(unittest.TestCase):
    def test_normal_defaults_preserve_legacy_and_explicit_new_choices(self):
        profile = 'graphics-world-aio-defaults'
        legacy = (b'[EQ2Graphics]\r\nShadowSoftness=0.2\r\nSunSoftness=6.94\r\n'
                  b'TextureQuestSunSoftnessPreferenceRealSun=6.94\r\n')
        after, journal = prepare_dlss_config(legacy, profile=profile)
        self.assertNotIn(b'PointShadowSoftness=', after)
        for line in legacy.splitlines()[1:]: self.assertIn(line + b'\r\n', after)
        restored, conflicts = restore_dlss_config(after, journal)
        self.assertEqual(restored, legacy); self.assertEqual(conflicts, ())
        explicit = legacy + b'PointShadowSoftness=2.75\r\n'
        after, _ = prepare_dlss_config(explicit, profile=profile)
        self.assertIn(b'PointShadowSoftness=2.75\r\n', after)
        self.assertNotIn(b'PointShadowSoftness=1.6', after)
        fresh, _ = prepare_dlss_config(b'[EQ2Graphics]\r\n', profile=profile)
        self.assertIn(b'PointShadowSoftness=1.6\r\n', fresh)


if __name__ == '__main__': unittest.main(verbosity=2)
