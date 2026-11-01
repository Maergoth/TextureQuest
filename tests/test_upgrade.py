"""Migration uses synthetic D-only game/cache files; no actual game actions."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import zipfile
import io
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import texturequest_upgrade as upgrade
import texturequest_graphics_proxy as graphics
from texture_swapper import core
from test_graphics_proxy import Fixture


class QuietReShade(Fixture):
    def setUp(self):
        super().setUp()
        (self.game / 'paks').mkdir(exist_ok=True)
        (self.game / 'paks/test.vpk').write_bytes(b'unchanged game archive')
        patcher = patch.object(core, 'validate_game', side_effect=lambda game: Path(game) / 'paks')
        patcher.start(); self.addCleanup(patcher.stop)

    def test_existing_effects_addons_and_hotkey_preserved_and_original_saved(self):
        raw = b'[GENERAL]\r\nPresetPath=my-preset.ini\r\n[ADDON]\r\nLoadFromDllMain=my-addon.addon64\r\n[INPUT]\r\nKeyOverlay=45,0,0,0\r\n[OVERLAY]\r\nTutorialProgress=0\r\nShowFPS=2\r\nCustomColour=1,2,3\r\n'
        (self.game / 'ReShade.ini').write_bytes(raw)
        self.install()
        changed = (self.game / 'ReShade.ini').read_bytes()
        self.assertIn(b'TutorialProgress=4', changed)
        self.assertIn(b'ShowFPS=0', changed)
        for value in (b'PresetPath=my-preset.ini', b'LoadFromDllMain=my-addon.addon64', b'KeyOverlay=36,0,0,0', b'CustomColour=1,2,3'):
            self.assertIn(value, changed)
        self.assertEqual(next((self.state / 'reshade-settings-history').glob('*.ini')).read_bytes(), raw)
        self.manager.uninstall(self.game)
        self.assertEqual((self.game / 'ReShade.ini').read_bytes(), changed)

    def test_new_install_has_no_tutorial_stats_and_home_available(self):
        self.install()
        raw = (self.game / 'ReShade.ini').read_bytes()
        self.assertIn(b'TutorialProgress=4', raw)
        self.assertIn(b'KeyOverlay=36,0,0,0', raw)
        self.assertIn(b'ShowFPS=0', raw)

    def test_old_disabled_overlay_key_becomes_home_and_rewrite_is_idempotent(self):
        raw = b'\xef\xbb\xbf[INPUT]\r\nKeyOverlay=0,0,0,0\r\n[OVERLAY]\r\nTutorialProgress=0\r\n'
        changed = graphics.quiet_reshade(raw)
        self.assertTrue(changed.startswith(b'\xef\xbb\xbf'))
        self.assertIn(b'KeyOverlay=36,0,0,0', changed)
        self.assertEqual(graphics.quiet_reshade(changed), changed)

    def test_old_hook_removed_automatically_without_touching_paks(self):
        self.proxy_170('installed')
        migration = upgrade.Upgrade(self.state, self.manager)
        self.assertTrue(migration.needed(self.game))
        original = (self.game / 'paks/test.vpk').read_bytes() if (self.game / 'paks/test.vpk').exists() else None
        with patch.object(upgrade, 'ensure_closed'):
            migration.before(self.game)
        self.assertFalse(self.manager.status(self.game)['installed'])
        self.assertFalse((self.game / 'd3d11.dll').exists())
        if original is not None: self.assertEqual((self.game / 'paks/test.vpk').read_bytes(), original)

    def test_changed_old_hook_is_preserved_and_migration_stops(self):
        self.proxy_170('installed')
        (self.game / 'd3d11.dll').write_bytes(b'user changed it')
        migration = upgrade.Upgrade(self.state, self.manager)
        with patch.object(upgrade, 'ensure_closed'), self.assertRaisesRegex(ValueError, 'Changed hook'):
            migration.before(self.game)
        self.assertEqual((self.game / 'd3d11.dll').read_bytes(), b'user changed it')


class CacheMigration(unittest.TestCase):
    def setUp(self):
        folder = Path(__file__).resolve().parents[1] / 'build'
        folder.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=folder, prefix='upgrade-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        from test_channels import fixture
        self.game, self.source, self.index, _, self.choices, self.opener, _ = fixture(self.root)
        self.state = self.root / 'data'
        self.graphics = MagicMock()
        self.graphics.legacy_in_use.return_value = False
        self.graphics.status.return_value = dict(installed=False, running=False)
        self.migration = upgrade.Upgrade(self.state, self.graphics)
        self.closed = patch.object(upgrade, 'ensure_closed'); self.closed.start(); self.addCleanup(self.closed.stop)

    def prepared(self):
        from texturequest_channels import ChannelStore
        return ChannelStore(self.state).prepare(self.source, self.game, self.choices, opener=self.opener)

    def test_old_replacement_requires_patcher_then_continues_after_repair(self):
        manager = upgrade.DirectSwapper(self.state)
        paks, path = manager._game_manifest(self.game)
        raw = (paks / 'test.vpk').read_bytes()
        row = dict(name='test.vpk', bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                   original_sha256='a' * 64, managed=False, status='unlocked')
        core._save(path, dict(schema=core.SCHEMA, paks_dir=str(paks), game_dir=str(self.game), status='inactive', archives=[row]))
        self.assertEqual(self.migration.installed_game(), str(self.game))
        with self.assertRaisesRegex(ValueError, 'Validate Game Assets'):
            self.migration.before(self.game)
        self.assertEqual((paks / 'test.vpk').read_bytes(), raw)
        self.graphics.uninstall.assert_not_called()
        (paks / 'test.vpk').write_bytes(b'patched original')
        self.migration.before(self.game)

    def test_verified_obsolete_cache_removed_new_objects_unknown_files_and_history_kept(self):
        prepared = self.prepared()
        objects = self.state / 'dds/objects/dds'
        old = b'DDS old synthetic cache'; digest = hashlib.sha256(old).hexdigest()
        obsolete = objects / (digest + '.dds'); obsolete.write_bytes(old)
        unknown = objects / ('a' * 64 + '.dds'); unknown.write_bytes(b'modified unknown file')
        current = list(objects.glob('*.dds'))[0]
        receipt = self.migration.complete(self.game, prepared)
        self.assertEqual(receipt['status'], 'complete')
        self.assertFalse(obsolete.exists())
        self.assertTrue(unknown.exists())
        self.assertTrue(current.exists())
        self.assertFalse(self.migration.needed(self.game))
        self.assertEqual(core._load(self.state / 'dds-prepared.json'), prepared)

    def test_changed_new_cache_refuses_cleanup(self):
        prepared = self.prepared()
        objects = self.state / 'dds/objects/dds'
        old = b'old'; digest = hashlib.sha256(old).hexdigest()
        obsolete = objects / (digest + '.dds'); obsolete.write_bytes(old)
        current = next(p for p in objects.glob('*.dds') if p != obsolete)
        current.write_bytes(b'broken')
        with self.assertRaises(ValueError): self.migration.complete(self.game, prepared)
        self.assertTrue(obsolete.exists())

    def test_old_direct_pak_downloads_removed_only_after_success(self):
        import texturequest_direct as direct
        from test_direct import fixture, Response
        blobs, manifest = fixture()
        validator = direct.validate_manifest
        with patch.object(direct, 'validate_manifest', side_effect=lambda v, **kw: validator(v, pinned=False)):
            manager = direct.DirectSwapper(self.state)
            previous = direct.prepare_direct(manager, manifest, opener=lambda request, **kw:
                Response(blobs[request.full_url.rsplit('/', 1)[1]], status=200))
            downloaded = [Path(row['path']) for row in previous['archives']]
            prepared = self.prepared()
            self.assertTrue(all(p.exists() for p in downloaded))
            self.migration.complete(self.game, prepared)
            self.assertFalse(any(p.exists() for p in downloaded))
            self.assertTrue((self.state / 'direct-prepared.json').exists())

    def test_full_dds_download_removed_from_exact_old_index_and_malformed_one_kept(self):
        old_raw = b'DDS invented original cached bytes'
        digest = hashlib.sha256(old_raw).hexdigest()
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w') as archive:
            archive.writestr('dds/' + digest + '.dds', old_raw)
        data = buffer.getvalue(); chunk_sha = hashlib.sha256(data).hexdigest()
        name = 'dds-001-' + chunk_sha[:12] + '.zip'
        index = dict(schema='texturequest-dds-index-v1', objects=[dict(entry='dds/' + digest + '.dds',
            sha256=digest, bytes=len(old_raw), chunk=name)])
        raw = json.dumps(index).encode(); index_sha = hashlib.sha256(raw).hexdigest()
        metadata = self.state / 'dds/metadata'; metadata.mkdir(parents=True)
        (metadata / (index_sha + '.json')).write_bytes(raw)
        downloads = self.state / 'dds/downloads'; downloads.mkdir()
        old = downloads / name; old.write_bytes(data)
        unknown = downloads / 'dds-002-aaaaaaaaaaaa.zip'; unknown.write_bytes(b'unknown')
        prepared = self.prepared()
        path = self.migration.receipt(self.game)
        core._save(path, dict(schema=upgrade.SCHEMA, game=str(self.game), status='preparing',
                             previous_dds=dict(index_sha256=index_sha)))
        self.migration.complete(self.game, prepared)
        self.assertFalse(old.exists())
        self.assertTrue(unknown.exists())

    def test_open_game_refuses_before_uninstall_or_cleanup(self):
        with patch.object(upgrade, 'ensure_closed', side_effect=ValueError('Close EQ2')):
            with self.assertRaisesRegex(ValueError, 'Close EQ2'): self.migration.before(self.game)
        self.graphics.uninstall.assert_not_called()

    def test_old_memory_loader_gets_time_to_close_without_second_click(self):
        loader = MagicMock()
        loader.status.return_value = dict(installed=True, running=True)
        loader.cache_in_use.side_effect = [True, True, False]
        migration = upgrade.Upgrade(self.state, self.graphics, loader)
        with patch.object(upgrade.time, 'sleep') as pause:
            migration.before(self.game)
        loader.uninstall.assert_called_once()
        self.assertEqual(pause.call_count, 2)

    def test_unresponsive_old_loader_refuses_without_force_kill(self):
        loader = MagicMock()
        loader.status.return_value = dict(installed=True, running=True)
        loader.cache_in_use.return_value = True
        migration = upgrade.Upgrade(self.state, self.graphics, loader)
        with patch.object(upgrade.time, 'monotonic', side_effect=[0, 11]), self.assertRaisesRegex(ValueError, 'did not close'):
            migration.before(self.game)
        self.graphics.uninstall.assert_not_called()

    def test_fresh_channel_download_does_not_masquerade_as_old_install(self):
        self.prepared()
        self.assertFalse(self.migration.needed(self.game))

    def test_another_game_install_keeps_shared_old_objects(self):
        prepared = self.prepared()
        old = b'cached for second game'; digest = hashlib.sha256(old).hexdigest()
        obsolete = self.state / 'dds/objects/dds' / (digest + '.dds'); obsolete.write_bytes(old)
        other = self.root / 'other-game'
        self.graphics._all_games.return_value = [str(other)]
        self.graphics.status.return_value = dict(installed=True, running=False)
        result = self.migration.complete(self.game, prepared)
        self.assertTrue(obsolete.exists())
        self.assertIn('Another installed game', result['cache_preserved'])

    def test_managed_replace_is_unlocked_for_patcher_without_deleting_game_bytes(self):
        manager = upgrade.DirectSwapper(self.state)
        paks, path = manager._game_manifest(self.game)
        target = paks / 'test.vpk'; raw = target.read_bytes()
        row = dict(name='test.vpk', bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                   original_sha256='a' * 64, managed=True, status='installed')
        core._save(path, dict(schema=core.SCHEMA, paks_dir=str(paks), game_dir=str(self.game), status='installed', archives=[row]))
        core._set_readonly(target, True)
        with patch.object(core, 'ensure_game_closed'), self.assertRaisesRegex(ValueError, 'Validate Game Assets'):
            self.migration.before(self.game)
        self.assertEqual(target.read_bytes(), raw)
        self.assertFalse(core._readonly(target))
        self.assertFalse(core._load(path)['archives'][0]['managed'])


if __name__ == '__main__': unittest.main()
