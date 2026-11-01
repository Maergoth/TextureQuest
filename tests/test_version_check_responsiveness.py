"""Small invented receipts and UI workers; no network, game or real cache reads."""
import copy
from pathlib import Path
import sys
import tempfile
import threading
import tkinter as tk
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from texture_swapper import core
import texturequest_app as app
import texturequest_channels as channels
import texturequest_dds_source as dds
from test_loose_dds_ui import GraphicsOnlyUI


def receipt(state, **changes):
    value = dict(schema='texturequest-loose-dds-prepared-v1', texture_version='1.3.0',
        source_sha256='1' * 64, index_sha256='2' * 64, choices_sha256='3' * 64,
        game=str(state / 'invented-game'), object_root=str(state / 'dds/objects'),
        objects=dict(path=str(state / 'dds/metadata/roster.json'), bytes=300, sha256='4' * 64),
        map=dict(path=str(state / 'dds/maps/map.bin'), bytes=256, sha256='5' * 64),
        object_count=23875, object_bytes=27788745153, named_textures=24693,
        distribution='generated-channels-only', coverage=dict(pending_groups=0))
    value.update(changes)
    return value


class CacheSummary(unittest.TestCase):
    def setUp(self):
        # Parent feature directory is on D:; no system or real user cache.
        self.temp = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[2])
        self.addCleanup(self.temp.cleanup)
        self.state = Path(self.temp.name)
        self.store = channels.ChannelStore(self.state)

    def save(self, **changes):
        value = receipt(self.state, **changes)
        core._save(self.state / 'dds-prepared.json', value)
        return value

    def test_summary_never_reads_roster_map_or_stats_objects(self):
        expected = self.save()
        original_stat = Path.stat
        def stat(path, *args, **kwargs):
            if path.is_relative_to(self.state / 'dds'):
                raise AssertionError('summary touched DDS inventory')
            return original_stat(path, *args, **kwargs)
        with patch.object(Path, 'stat', stat), \
             patch.object(dds.DDSStore, 'verify', side_effect=AssertionError('verify called')), \
             patch.object(dds.DDSStore, 'object_rows', side_effect=AssertionError('roster called')), \
             patch.object(dds, 'pin_file', side_effect=AssertionError('payload file read')):
            self.assertEqual(self.store.summary(), expected)
        self.assertFalse((self.state / 'dds').exists())

    def test_summary_preserves_generated_only_distribution_rule(self):
        self.save(distribution='legacy-full-dds')
        self.assertIsNotNone(dds.DDSStore(self.state).summary())
        self.assertIsNone(self.store.summary())

    def test_summary_refuses_bad_checksum_and_invalid_count(self):
        self.save(object_count=True)
        self.assertIsNone(self.store.summary())
        self.save()
        path = self.state / 'dds-prepared.json'
        path.write_bytes(path.read_bytes().replace(b'1.3.0', b'1.3.1'))
        self.assertIsNone(self.store.summary())

    def test_summary_bounds_receipt_before_json_loading(self):
        (self.state / 'dds-prepared.json').write_bytes(b' ' * (64 * 1024 + 1))
        with patch.object(core, '_load', side_effect=AssertionError('oversize receipt read')):
            self.assertIsNone(self.store.summary())

    def test_saved_default_still_verifies_inventory(self):
        expected = self.save()
        with patch.object(self.store, 'verify') as verify:
            self.assertEqual(self.store.saved(), expected)
        verify.assert_called_once_with(expected, full=False)


class ResponsiveVersionUI(GraphicsOnlyUI):
    # Focused runner selects only the new names below. Inherited historical
    # tests retain their original scopes and are not new-case evidence.
    def result(self):
        return dict(installer=dict(status='verified', version=app.APP_VERSION,
                                  update_available=False, local_newer=False),
                    pack=dict(status='verified', version=app.texture_version(self.ui.release),
                              manifest=self.ui.release, identity=app.pack_identity(self.ui.release),
                              update_available=False))

    def save_summary(self):
        value = receipt(self.state)
        core._save(self.state / 'dds-prepared.json', value)
        return value

    def test_startup_uses_summary_without_saved_inventory_or_fresh_check(self):
        value = self.save_summary()
        window = tk.Toplevel(self.tk)
        window.withdraw()
        self.addCleanup(window.destroy)
        with patch.object(channels.ChannelStore, 'saved', side_effect=AssertionError('saved inventory called')), \
             patch.object(channels.ChannelStore, 'verify', side_effect=AssertionError('verify called')):
            ui = app.TextureQuestApp(window, self.state, self.game, startup_check=False)
        self.assertEqual(ui.prepared, value)
        self.assertFalse(ui.release_checked)
        self.assertIn('Saved texture cache', ui.pack_versions_label.cget('text'))
        self.assertNotIn('verified', ui.download_label.cget('text'))
        self.assertNotIn('Latest', ui.pack_update_label.cget('text'))

    def test_explicit_verify_defers_full_inventory_check_to_worker(self):
        value = self.save_summary()
        with patch.object(self.ui, 'begin') as begin, patch.object(self.ui.dds_store, 'verify') as verify:
            self.ui.verify_installation()
            verify.assert_not_called()
            self.assertEqual(begin.call_args.args[0], 'verify')
            begin.call_args.args[2]()
            verify.assert_called_once_with(value, full=True)

    def test_update_metadata_branches_start_concurrently(self):
        barrier = threading.Barrier(2, timeout=5)
        expected = self.result()
        source = dict(texture_version='1.3.0')
        def versions(*args):
            barrier.wait()
            return copy.deepcopy(expected)
        def texture_source(**kwargs):
            barrier.wait()
            return source
        with patch.object(app, 'check_versions', side_effect=versions) as versions_call, \
             patch.object(app, 'fetch_dds_source', side_effect=texture_source) as source_call, \
             patch.object(self.ui, 'begin') as begin:
            self.ui.check_updates()
            result = begin.call_args.args[2]()
        self.assertEqual(result, dict(expected, dds_source=source))
        versions_call.assert_called_once_with(self.ui.release, self.ui.app_version, self.state)
        source_call.assert_called_once_with(minimum='1.3.0')
        partials = [self.ui.events.get_nowait(), self.ui.events.get_nowait()]
        self.assertEqual([row[0] for row in partials], ['version-progress', 'version-progress'])

    def test_partial_verified_result_is_visible_while_other_check_runs(self):
        self.ui.busy = 'check'
        installer = self.result()['installer']
        self.ui.events.put(('version-progress', dict(installer=installer)))
        with patch.object(self.ui, 'verify_installation', side_effect=AssertionError('automatic verify')):
            self.ui.poll()
        self.assertEqual(self.ui.busy, 'check')
        self.assertEqual(self.ui.update_check['installer'], installer)
        self.assertIn('current', self.ui.app_update_label.cget('text'))

    def test_successful_version_check_does_not_start_inventory_verification(self):
        self.ui.events.put(('done', 'check', self.result()))
        with patch.object(self.ui, 'upgrade_pending', return_value=False), \
             patch.object(self.ui, 'verify_installation', side_effect=AssertionError('automatic verify')), \
             patch.object(self.ui.dds_store, 'saved', side_effect=AssertionError('inventory called')):
            self.ui.poll()
        self.assertTrue(self.ui.release_checked)
        self.assertIsNone(self.ui.busy)

    def test_failed_check_keeps_summary_without_claiming_fresh_verification(self):
        expected = self.save_summary()
        unavailable = dict(status='unavailable', message='Invented offline result')
        self.ui.events.put(('done', 'check', dict(installer=unavailable, pack=unavailable)))
        with patch.object(self.ui.dds_store, 'saved', side_effect=AssertionError('inventory called')), \
             patch.object(self.ui, 'verify_installation', side_effect=AssertionError('automatic verify')):
            self.ui.poll()
        self.assertEqual(self.ui.prepared, expected)
        self.assertFalse(self.ui.release_checked)
        self.assertNotIn('verified', self.ui.download_label.cget('text'))

    def test_verified_check_continues_only_ack_armed_hook_upgrade(self):
        # A passive check cannot arm setup; a validated successful app-update
        # startup is the one-shot authority retained for automatic refresh.
        self.ui.post_update_setup = True
        self.ui.events.put(('done', 'check', self.result()))
        with patch.object(self.ui, 'upgrade_pending', return_value=True), \
             patch.object(self.ui, '_download_latest') as upgrade, \
             patch.object(self.ui, 'verify_installation', side_effect=AssertionError('automatic verify')):
            self.ui.poll()
        upgrade.assert_called_once_with(auto_install=True, upgrading=True)
        self.assertFalse(self.ui.post_update_setup)

    def test_malformed_texture_descriptor_preserves_successful_app_result(self):
        expected = self.result()
        with patch.object(app, 'check_versions', return_value=expected), \
             patch.object(app, 'fetch_dds_source', side_effect=KeyError('invented missing texture field')), \
             patch.object(self.ui, 'begin') as begin:
            self.ui.check_updates()
            result = begin.call_args.args[2]()
        self.assertEqual(result['installer'], expected['installer'])
        self.assertEqual(result['pack'], expected['pack'])
        self.assertIn('invented missing texture field', result['dds_error'])

    def test_unexpected_app_failure_preserves_successful_texture_result(self):
        source = dict(texture_version='1.3.0')
        with patch.object(app, 'check_versions', side_effect=TypeError('invented app metadata failure')), \
             patch.object(app, 'fetch_dds_source', return_value=source), \
             patch.object(self.ui, 'begin') as begin:
            self.ui.check_updates()
            result = begin.call_args.args[2]()
        self.assertEqual(result['dds_source'], source)
        self.assertEqual(result['installer']['status'], 'unavailable')
        self.assertEqual(result['pack']['status'], 'unavailable')


if __name__ == '__main__':
    unittest.main()
