"""Normal direct installer performance and recovery; own D fixtures only."""
import copy
import os
from pathlib import Path
import unittest
from unittest.mock import patch

import test_direct as td
import texturequest_fast_install as fast
import texturequest_processes as processes
import texturequest_direct as direct
from texture_swapper import core


class FastInstallTests(unittest.TestCase):
    opener = td.DirectTests.opener
    prepare = td.DirectTests.prepare
    legacy = td.DirectTests.legacy

    def setUp(self):
        td.DirectTests.setUp(self)
        self.game = self.folder / 'fixture-game-only'
        self.paks = self.game / 'paks'
        self.paks.mkdir(parents=True)
        (self.game / 'EverQuest2.exe').write_text('own fixture marker; never launched')
        for name in self.blobs:
            (self.paks / name).write_bytes(td.vpk(b'old fixture bytes'))

    def install(self, ready=None):
        with patch.object(core, 'ensure_game_closed', return_value=None):
            return self.swapper.install(ready or self.ready, self.game)

    def test_direct_ready_never_decompresses_known_resources(self):
        with patch.object(core, 'validate_archive', side_effect=AssertionError('resource scan')), patch.object(core, '_payload', side_effect=AssertionError('payload inflate')):
            ready = self.prepare()
            self.requests.clear()
            reused = self.prepare()
        self.assertFalse(self.requests)
        self.assertEqual(ready['archives'], reused['archives'])
        self.assertEqual({Path(r['path']).parent for r in ready['archives']}, {self.swapper.state_dir / 'downloads'})

    def test_copy_only_streamed_hash_no_pack_target_stage_status_reads(self):
        self.ready = self.prepare()
        copied = []
        original = fast.copy_archive
        def counted(source, output, row):
            copied.append(row['name'])
            return original(source, output, row)
        with patch.object(fast, 'copy_archive', counted), patch.object(core, '_hash', side_effect=AssertionError('redundant full hash')), patch.object(core, 'validate_archive', side_effect=AssertionError('resource scan')):
            result = self.install()
            observed = self.swapper.status(self.game)
        self.assertEqual(copied, list(self.blobs))
        self.assertEqual(result['intact_count'], len(self.blobs))
        self.assertEqual(observed['status'], 'installed')
        for name, blob in self.blobs.items():
            self.assertEqual((self.paks / name).read_bytes(), blob)
        self.assertTrue(all(r['verification'] == 'recorded-file-metadata' for r in observed['archives']))

    def test_known_repeat_skips_but_same_size_changed_target_recopies(self):
        self.ready = self.prepare()
        self.install()
        identities = {name: (self.paks / name).stat().st_ino for name in self.blobs}
        with patch.object(fast, 'copy_archive', side_effect=AssertionError('should skip')):
            self.install()
        self.assertEqual(identities, {name: (self.paks / name).stat().st_ino for name in self.blobs})
        name = next(iter(self.blobs))
        target = self.paks / name
        changed = bytearray(target.read_bytes())
        changed[7] ^= 1
        target.write_bytes(changed)
        self.assertEqual(target.stat().st_size, len(self.blobs[name]))
        self.assertEqual(self.swapper.status(self.game)['changed_count'], 1)
        copied = []
        original = fast.copy_archive
        def counted(source, output, row):
            copied.append(row['name'])
            return original(source, output, row)
        with patch.object(fast, 'copy_archive', counted):
            self.install()
        self.assertEqual(copied, [name])
        self.assertEqual(target.read_bytes(), self.blobs[name])

    def test_corrupt_source_rejected_before_target_replace(self):
        self.ready = self.prepare()
        row = self.ready['archives'][0]
        source = Path(row['path'])
        source.write_bytes(b'x' * row['bytes'])
        original = (self.paks / row['name']).read_bytes()
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.install()
        self.assertEqual((self.paks / row['name']).read_bytes(), original)
        self.assertEqual(self.swapper.status(self.game)['recorded_status'], 'interrupted')

    def interrupt(self):
        self.ready = self.prepare()
        original = (self.paks / self.ready['archives'][0]['name']).read_bytes()
        def failed_copy(source, output, row):
            output.write(source.read(16))
            output.flush()
            os.fsync(output.fileno())
            raise OSError('own interrupted copy')
        with patch.object(fast, 'copy_archive', failed_copy), self.assertRaisesRegex(OSError, 'interrupted'):
            self.install()
        _, path = self.swapper._game_manifest(self.game)
        saved = core._load(path)
        row = saved['archives'][0]
        staged = self.paks / row['staged_name']
        self.assertEqual(staged.stat().st_size, 16)
        self.assertEqual((self.paks / row['name']).read_bytes(), original)
        self.assertEqual(saved['status'], 'interrupted')
        return staged

    def test_interrupted_partial_retry_keeps_target_then_recopies(self):
        staged = self.interrupt()
        self.install()
        self.assertFalse(staged.exists())
        self.assertEqual(self.swapper.status(self.game)['intact_count'], len(self.blobs))

    def test_replaced_partial_is_preserved_and_refused(self):
        staged = self.interrupt()
        saved = staged.with_suffix('.preserved')
        staged.rename(saved)
        staged.write_bytes(b'unknown replacement')
        with self.assertRaisesRegex(ValueError, 'preserved'):
            self.install()
        self.assertEqual(staged.read_bytes(), b'unknown replacement')
        self.assertEqual(saved.stat().st_size, 16)

    def test_game_starts_during_copy_refuses_commit_and_retry_recovers(self):
        self.ready = self.prepare()
        original = (self.paks / self.ready['archives'][0]['name']).read_bytes()
        with patch.object(core, 'ensure_game_closed', side_effect=[None, RuntimeError('game active')]), self.assertRaisesRegex(RuntimeError, 'active'):
            self.swapper.install(self.ready, self.game)
        self.assertEqual((self.paks / self.ready['archives'][0]['name']).read_bytes(), original)
        self.install()
        self.assertEqual(self.swapper.status(self.game)['intact_count'], len(self.blobs))

    def test_lost_commit_ack_reuses_exact_recorded_target(self):
        self.ready = self.prepare()
        original = fast._commit
        def applied_then_error(*args):
            original(*args)
            raise OSError('own lost commit acknowledgment')
        with patch.object(fast, '_commit', applied_then_error), self.assertRaisesRegex(OSError, 'acknowledgment'):
            self.install()
        copied = []
        original_copy = fast.copy_archive
        def counted(source, output, row):
            copied.append(row['name'])
            return original_copy(source, output, row)
        with patch.object(fast, 'copy_archive', counted):
            self.install()
        self.assertEqual(copied, list(self.blobs)[1:])
        self.assertEqual(self.swapper.status(self.game)['intact_count'], len(self.blobs))

    def test_explicit_verify_still_hashes_installed_bytes(self):
        self.ready = self.prepare()
        self.install()
        original = core._hash
        read = []
        def counted(path):
            read.append(str(path))
            return original(path)
        with patch.object(core, '_hash', counted):
            result = self.swapper.status(self.game, verify=True)
        self.assertEqual(len(read), len(self.blobs))
        self.assertEqual(result['intact_count'], len(self.blobs))
        self.assertTrue(all(r['actual_sha256'] is not None for r in result['archives']))

    def test_recorded_repeat_clears_legacy_readonly_attribute(self):
        self.ready = self.prepare()
        self.install()
        target = self.paks / next(iter(self.blobs))
        core._set_readonly(target, True)
        with patch.object(fast, 'copy_archive', side_effect=AssertionError('unchanged copy should skip')):
            self.install()
        self.assertFalse(core._readonly(target))

    def test_fresh_download_streamed_sha_has_no_full_file_rescan(self):
        with patch.object(direct.release, 'hash_stream', side_effect=AssertionError('fresh download should not rescan')):
            ready = self.prepare()
        self.assertEqual(len(self.requests), len(self.blobs))
        self.assertEqual(len(ready['archives']), len(self.blobs))

    def test_existing_legacy_ready_install_needs_no_network_or_migration(self):
        ready = self.legacy()
        self.swapper.trusted_manifest = self.manifest
        old_paths = [Path(row['path']) for row in ready['archives']]
        self.assertEqual(self.swapper.saved_ready(self.manifest), ready)
        self.assertFalse((self.swapper.state_dir / 'downloads').exists())
        with patch.object(core, '_hash', side_effect=AssertionError('redundant hash')), patch.object(core, 'validate_archive', side_effect=AssertionError('resource validation')):
            self.install(ready)
        self.assertTrue(all(path.exists() for path in old_paths))
        self.assertFalse((self.swapper.state_dir / 'downloads').exists())
        self.assertEqual(self.swapper.status(self.game)['intact_count'], len(self.blobs))

    def test_changed_legacy_receipt_identity_never_becomes_trusted_ready(self):
        self.legacy()
        changed = copy.deepcopy(self.manifest)
        changed['archives'][0]['sha256'] = 'f' * 64
        self.assertIsNone(self.swapper.saved_ready(changed))

    def test_runtime_fixture_guard_cannot_accept_another_game_path(self):
        from texturequest_runtime_check import _fixture_closed_guard
        original = core.ensure_game_closed
        with self.assertRaisesRegex(ValueError, 'real game path'):
            with _fixture_closed_guard(self.folder, self.swapper, self.game):
                self.fail('wrong fixture path accepted')
        self.assertIs(core.ensure_game_closed, original)


class EnvelopeTests(unittest.TestCase):
    def test_app_envelope_preserves_all_310_texture_identities_and_urls(self):
        import json
        from texturequest_version import APP_VERSION
        root = Path(__file__).resolve().parents[1]
        old = json.loads((root / 'release-manifest-1.3.0.json').read_bytes())
        new = json.loads((root / 'release-manifest-1.3.2.json').read_bytes())
        self.assertEqual(APP_VERSION, '1.3.2')
        self.assertEqual(new['schema'], 'texturequest-release-v3')
        self.assertEqual(new['texture_version'], '1.2.0')
        self.assertEqual(new['archives'], old['archives'])
        self.assertEqual(len(new['archives']), 310)
        self.assertEqual((root / 'src/release-manifest.json').read_bytes(), (root / 'release-manifest-1.3.2.json').read_bytes())


class ProcessTests(unittest.TestCase):
    def test_native_guard_names_casefold_and_all_aliases(self):
        for name in processes.GUARDED:
            with self.subTest(name=name), patch.object(processes, 'process_names', return_value=['python.exe', name.upper()]), self.assertRaisesRegex(RuntimeError, 'Close EverQuest'):
                processes.ensure_closed()
        with patch.object(processes, 'process_names', return_value=['python.exe']):
            processes.ensure_closed()

    def test_native_empty_or_failed_census_fails_closed(self):
        with patch.object(processes, 'process_names', return_value=[]), self.assertRaisesRegex(RuntimeError, 'Unable'):
            processes.ensure_closed()
        with patch.object(processes, 'process_names', side_effect=OSError('own failed census')), self.assertRaisesRegex(RuntimeError, 'Unable'):
            processes.ensure_closed()

    def test_actual_readonly_native_census(self):
        names = processes.process_names()
        self.assertTrue(names)
        self.assertTrue(any(name.casefold() == 'python.exe' for name in names))


if __name__ == '__main__':
    unittest.main()
