"""New second-backend admission/lease tests with invented D-only payloads."""
from pathlib import Path
import hashlib
import json
import tempfile
import unittest
from unittest.mock import patch

from test_graphics_proxy import Fixture, ADDON_BUILD
from test_dlss_install import enable_dlss_fixture
import texturequest_components as components
import texturequest_graphics_proxy as graphics
import texturequest_dlss as settings


def use_feeder(runtime):
    path = runtime / 'RUNTIME.json'
    doc = json.loads(path.read_bytes())
    doc['files'] = [row for row in doc['files'] if row['name'] != 'effects/TQEffects.fx']
    path.write_text(json.dumps(doc))
    enable_dlss_fixture(runtime)


def use_aio(runtime):
    path = runtime / 'RUNTIME.json'
    doc = json.loads(path.read_bytes())
    rows = []
    for role, name in graphics.AIO_ROLES.items():
        target = runtime / name
        target.parent.mkdir(parents=True, exist_ok=True)
        raw = ('invented AIO role ' + role).encode()
        target.write_bytes(raw)
        rows.append(dict(role=role, name=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
    doc['dlss'] = dict(schema='texturequest-dlss-runtime-v1', id=graphics.AIO_PROFILE_ID,
                       private=True, api='d3d11', files=rows)
    path.write_text(json.dumps(doc))


class AIOSettings(unittest.TestCase):
    def test_separate_lease_changes_only_exact_standalone_scalars(self):
        raw = b'\xef\xbb\xbf[GENERAL]\r\nPreprocessorDefinitions=DLSS5_MV_PROVIDER=0,MY_MACRO=7\r\n[RenoDX.DLSS5]\r\nEnableHooks=9\r\n[Standalone.DLSSNR]\r\nFrameGeneration = 0 ; own note\r\nWindowedInputScaling=1\r\nUnknown=\xc3\xbc'
        after, lease = settings.prepare_dlss_config(raw, profile='aio')
        self.assertEqual(lease['schema'], settings.AIO_SCHEMA)
        self.assertIn(b'FrameGeneration = 1 ; own note', after)
        self.assertIn(b'WindowedInputScaling=0', after)
        self.assertIn(b'DLSS5_MV_PROVIDER=0,MY_MACRO=7', after)
        self.assertIn(b'EnableHooks=9', after)
        self.assertNotIn(b'NRStyle=', after)
        self.assertEqual(settings.restore_dlss_config(after, lease), (raw, ()))
        self.assertEqual(len(settings.AIO_VALUES), 18)

    def test_aio_later_edit_survives_and_feeder_journal_remains_supported(self):
        raw = b'[Standalone.DLSSNR]\nFrameGeneration=0\n'
        after, lease = settings.prepare_dlss_config(raw, profile='aio')
        restored, conflicts = settings.restore_dlss_config(after.replace(b'FrameGeneration=1', b'FrameGeneration=7'), lease)
        self.assertIn(b'FrameGeneration=7', restored)
        self.assertIn('Standalone.DLSSNR.FrameGeneration', conflicts)
        feeder, old = settings.prepare_dlss_config(raw)
        self.assertEqual(old['schema'], settings.SCHEMA)
        self.assertIn(b'DLSS5_MV_PROVIDER=3', feeder)
        self.assertIn(b'NRStyle=0', feeder)
        self.assertEqual(settings.restore_dlss_config(feeder, old), (raw, ()))

    def test_aio_duplicate_and_wrong_backend_refuse(self):
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            settings.prepare_dlss_config(b'[Standalone.DLSSNR]\nEnabled=0\nEnabled=1\n', profile='aio')
        with self.assertRaisesRegex(ValueError, 'Unknown'):
            settings.prepare_dlss_config(b'', profile='unknown')
        after, lease = settings.prepare_dlss_config(b'', profile='aio')
        lease['schema'] = settings.SCHEMA
        with self.assertRaisesRegex(ValueError, 'checksum'):
            settings.restore_dlss_config(after, lease)


class AIOInstall(Fixture):
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / 'build/aio-fixtures-001'
        base.mkdir(parents=True, exist_ok=True)
        local = patch.object(tempfile, 'tempdir', str(base)); local.start(); self.addCleanup(local.stop)
        super().setUp()
        enable_dlss_fixture(self.runtime)
        self.selected = dict(components.normalize(), dlss=True)

    def test_aio_bundled_id_selects_five_modules_two_shaders_and_backend_flag(self):
        use_aio(self.runtime)
        self.assertEqual(self.manager.status(self.game)['dlss_backend'], 'aio')
        self.install(components=self.selected)
        record = self.record()
        self.assertEqual(record['dlss_profile'], graphics.AIO_PROFILE_ID)
        self.assertEqual(record['dlss_backend'], 'aio')
        self.assertIn(b'DLSSBackend=aio', (self.game / 'TextureQuest.ini').read_bytes())
        for role in graphics.AIO_MODULES:
            name = Path(graphics.AIO_ROLES[role]).name
            self.assertEqual((self.game / name).read_bytes(), (self.runtime / graphics.AIO_ROLES[role]).read_bytes())
        self.assertEqual(len(list((self.state / 'dlss').glob('**/*.*'))), 2)
        ini = (self.game / 'ReShade.ini').read_bytes()
        for (_, key), value in settings.AIO_VALUES.items(): self.assertIn((key + '=' + value).encode(), ini)
        self.assertNotIn(b'RenoDX.DLSS5', ini); self.assertNotIn(b'DLSS5_MV_PROVIDER', ini)
        self.assertFalse((self.game / 'renodx-dlss5.addon64').exists())
        self.assertTrue(self.manager.status(self.game)['dlss_installed'])

    def test_foreign_fg_bridge_and_mixed_profile_refuse_before_mutation(self):
        use_aio(self.runtime)
        for name in ('nvngx.dll', 'nvngx_dlssg.dll', 'renodx-dlss5.addon64'):
            (self.game / name).write_bytes(b'foreign')
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'exists|Another DLSS'):
                self.manager.preflight_components(self.game, self.selected)
            self.assertEqual((self.game / name).read_bytes(), b'foreign')
            (self.game / name).unlink()
        path = self.runtime / 'RUNTIME.json'; doc = json.loads(path.read_bytes())
        doc['dlss']['files'][0]['role'] = 'feeder'
        path.write_text(json.dumps(doc))
        with self.assertRaisesRegex(ValueError, 'unsafe DLSS role'):
            self.install(components=self.selected)
        self.manager._activate_prepared.assert_not_called()
        self.assertFalse((self.game / 'TextureQuest.addon64').exists())

    def test_owned_feeder_to_aio_refresh_preserves_configs_and_separates_leases(self):
        (self.game / 'dxgi.dll').write_bytes(ADDON_BUILD)
        original = b'[GENERAL]\nPreprocessorDefinitions=USER=7\n[RenoDX.DLSS5]\nEnableHooks=9\n[Standalone.DLSSNR]\nFrameGeneration=0\n'
        (self.game / 'ReShade.ini').write_bytes(original)
        cfg = self.game / 'dlss5-feed.cfg'; cfg.write_bytes(b'upstream user runtime configuration')
        self.install(components=self.selected)
        use_aio(self.runtime)
        status = self.manager.status(self.game)
        self.assertTrue(status['refresh_required']); self.assertEqual(status['dlss_backend'], 'feeder')
        self.manager.preflight_components(self.game, self.selected)
        self.manager.uninstall(self.game)
        self.assertIn(b'EnableHooks=9', (self.game / 'ReShade.ini').read_bytes())
        self.install(components=self.selected)
        self.assertFalse((self.game / 'dlss5-feed.addon64').exists())
        self.assertFalse((self.game / 'renodx-dlss5.addon64').exists())
        self.assertFalse(self.manager.status(self.game)['refresh_required'])
        self.assertEqual(self.record()['dlss_consumer_settings']['journal']['schema'], settings.AIO_SCHEMA)
        self.manager.uninstall(self.game)
        restored = (self.game / 'ReShade.ini').read_bytes()
        self.assertIn(b'FrameGeneration=0', restored); self.assertIn(b'EnableHooks=9', restored)
        self.assertIn(b'PreprocessorDefinitions=USER=7', restored)
        self.assertEqual(cfg.read_bytes(), b'upstream user runtime configuration')

    def test_owned_aio_to_feeder_transition_removes_only_old_owned_modules(self):
        use_aio(self.runtime); self.install(components=self.selected)
        use_feeder(self.runtime)
        self.assertTrue(self.manager.status(self.game)['refresh_required'])
        self.manager.preflight_components(self.game, self.selected)
        self.manager.uninstall(self.game); self.install(components=self.selected)
        self.assertIn(b'DLSSBackend=feeder', (self.game / 'TextureQuest.ini').read_bytes())
        self.assertFalse((self.game / 'nvngx.dll').exists())
        self.assertFalse((self.game / 'nvngx_dlssg.dll').exists())
        self.assertFalse((self.game / 'standalone-dlssnr.addon64').exists())
        self.assertTrue((self.game / 'renodx-dlss5.addon64').exists())

    def test_changed_old_backend_file_blocks_transition_and_keeps_working_record(self):
        self.install(components=self.selected)
        path = self.game / 'dlss5-feed.addon64'; path.write_bytes(b'external changed addon')
        before = self.manager._record(self.game)
        use_aio(self.runtime)
        with self.assertRaisesRegex(ValueError, 'Another DLSS'):
            self.manager.preflight_components(self.game, self.selected)
        self.assertEqual(self.manager._record(self.game), before)
        self.assertEqual(path.read_bytes(), b'external changed addon')
        self.assertFalse((self.game / 'standalone-dlssnr.addon64').exists())


if __name__ == '__main__': unittest.main()
