"""New hook-refresh cases on invented D-only metadata and DLL fixtures."""
from pathlib import Path
import hashlib
import tempfile
from unittest.mock import patch

from test_graphics_proxy import Fixture, ADDON_BUILD
import texturequest_graphics_proxy as graphics
import texturequest_upgrade as upgrade
from texture_swapper import core


class HookRefresh(Fixture):
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / 'build/hook-refresh-fixtures'
        base.mkdir(parents=True, exist_ok=True)
        temporary = patch.object(tempfile, 'tempdir', str(base)); temporary.start(); self.addCleanup(temporary.stop)
        super().setUp()
        self.version = patch.object(graphics, 'APP_VERSION', '7.0.1'); self.version.start(); self.addCleanup(self.version.stop)
        stamp = patch.object(upgrade, 'VERSION', '7.0.1'); stamp.start(); self.addCleanup(stamp.stop)
        self.migration = upgrade.Upgrade(self.state, self.manager)
        # This scope checks refresh metadata, not stock-archive inspection.
        stock = patch.object(self.migration, '_stock'); stock.start(); self.addCleanup(stock.stop)
        closed = patch.object(upgrade, 'ensure_closed'); closed.start(); self.addCleanup(closed.stop)

    def old_stamp(self, version='7.0.0'):
        record = self.manager._record(self.game)
        if version is None: record.pop('app_version', None)
        else: record['app_version'] = version
        self.manager._save(self.game, record)

    def completed(self):
        core._save(self.migration.receipt(self.game), dict(schema=upgrade.SCHEMA, game=str(self.game),
                                                         version='7.0.1', status='complete'))

    def test_install_records_launcher_and_exact_runtime_descriptor(self):
        self.install()
        record = self.manager._record(self.game)
        self.assertEqual(record['app_version'], '7.0.1')
        expected = hashlib.sha256((self.runtime/'RUNTIME.json').read_bytes()).hexdigest()
        self.assertEqual(record['runtime_descriptor_sha256'], expected)
        self.assertEqual(self.manager.status(self.game)['installed_app_version'], '7.0.1')
        self.assertFalse(self.manager.refresh_needed(self.game))

    def test_completed_receipt_cannot_hide_older_same_dll_hook(self):
        self.install(); self.completed(); self.old_stamp()
        before = (self.game/'TextureQuest.addon64').read_bytes()
        self.assertEqual(before, (self.runtime/'TextureQuest.addon64').read_bytes())
        self.assertTrue(self.manager.refresh_needed(self.game))
        self.assertTrue(self.migration.needed(self.game))

    def test_normal_refresh_reapplies_quiet_defaults_and_completes_once(self):
        self.install(); self.completed(); self.old_stamp()
        ini = self.game/'ReShade.ini'
        ini.write_bytes(ini.read_bytes().replace(b'TutorialProgress=4', b'TutorialProgress=0') + b'[CUSTOM]\nValue=keep\n')
        prior = self.manager._record(self.game)['id']
        self.assertTrue(self.migration.needed(self.game))
        self.migration.before(self.game)
        self.assertFalse(self.manager.status(self.game)['installed'])
        self.install(); self.completed()
        self.assertNotEqual(prior, self.manager._record(self.game)['id'])
        self.assertIn(b'TutorialProgress=4', ini.read_bytes())
        self.assertIn(b'Value=keep', ini.read_bytes())
        self.assertFalse(self.manager.refresh_needed(self.game))
        self.assertFalse(self.migration.needed(self.game))
        record = self.manager._record(self.game)
        self.assertFalse(self.migration.needed(self.game))
        self.assertEqual(self.manager._record(self.game), record)

    def test_unstamped_legacy_managed_hook_requires_one_refresh(self):
        self.install(); self.completed(); self.old_stamp(None)
        status = self.manager.status(self.game)
        self.assertIsNone(status['installed_app_version'])
        self.assertTrue(status['refresh_required'])
        self.assertTrue(self.migration.needed(self.game))
        self.migration.before(self.game); self.install(); self.completed()
        self.assertFalse(self.migration.needed(self.game))

    def test_newer_hook_is_never_automatically_downgraded(self):
        self.install(); self.old_stamp('7.0.2')
        record = self.manager._record(self.game)
        self.assertTrue(self.manager.status(self.game)['installed_newer_app'])
        self.assertFalse(self.manager.refresh_needed(self.game))
        self.assertFalse(self.migration.needed(self.game))
        with self.assertRaisesRegex(ValueError, 'newer'):
            self.migration.before(self.game)
        self.assertEqual(self.manager._record(self.game), record)
        self.assertFalse(self.migration.receipt(self.game).exists())

    def test_changed_native_hook_is_preserved_and_refresh_stops(self):
        self.install(); self.old_stamp(); self.completed()
        addon = self.game/'TextureQuest.addon64'; addon.write_bytes(b'unknown user changed native file')
        with self.assertRaisesRegex(ValueError, 'Changed hook'):
            self.migration.before(self.game)
        self.assertEqual(addon.read_bytes(), b'unknown user changed native file')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.install()
        self.assertEqual(addon.read_bytes(), b'unknown user changed native file')

    def test_game_open_refuses_before_refresh_mutation(self):
        self.install(); self.old_stamp(); self.completed()
        record = self.manager._record(self.game)
        raw = (self.game/'TextureQuest.addon64').read_bytes()
        with patch.object(upgrade, 'ensure_closed', side_effect=ValueError('Close EQ2')):
            with self.assertRaisesRegex(ValueError, 'Close EQ2'):
                self.migration.before(self.game)
        self.assertEqual(self.manager._record(self.game), record)
        self.assertEqual((self.game/'TextureQuest.addon64').read_bytes(), raw)

    def test_existing_recognized_reshade_without_ini_gets_owned_defaults(self):
        dll = self.game/'dxgi.dll'; dll.write_bytes(ADDON_BUILD)
        self.install()
        record = self.manager._record(self.game)
        names = {Path(row['path']).name for row in record['files']}
        self.assertIn('ReShade.ini', names)
        self.assertNotIn('dxgi.dll', names)
        self.assertFalse(record['reshade_owned'])
        self.assertIn(b'TutorialProgress=4', (self.game/'ReShade.ini').read_bytes())
        self.assertEqual(dll.read_bytes(), ADDON_BUILD)
        self.manager.uninstall(self.game)
        self.assertEqual(dll.read_bytes(), ADDON_BUILD)

    def test_malformed_stamp_preserves_record_and_refuses(self):
        self.install(); self.old_stamp('invalid version')
        raw = self.manager._record_path(self.game).read_bytes()
        with self.assertRaisesRegex(ValueError, 'Unsupported TextureQuest version'):
            self.manager.status(self.game)
        self.assertEqual(self.manager._record_path(self.game).read_bytes(), raw)

    def test_explicit_null_stamp_is_not_missing_legacy_metadata(self):
        self.install()
        record = self.manager._record(self.game); record['app_version'] = None
        self.manager._save(self.game, record)
        raw = self.manager._record_path(self.game).read_bytes()
        with self.assertRaisesRegex(ValueError, 'Unsupported TextureQuest version'):
            self.manager.refresh_needed(self.game)
        self.assertEqual(self.manager._record_path(self.game).read_bytes(), raw)
