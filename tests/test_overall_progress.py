"""Cumulative progress and actual UI events using disposable D fixtures."""
from pathlib import Path
import threading
import unittest
from unittest.mock import patch

import test_fast_install as fast_tests
import test_direct_ui as ui_tests
from texturequest_progress import OverallProgress
import texturequest_fast_install as fast
from texture_swapper import core


class CopyProgress(unittest.TestCase):
    setUp = fast_tests.FastInstallTests.setUp
    opener = fast_tests.FastInstallTests.opener
    prepare = fast_tests.FastInstallTests.prepare

    def test_copied_bytes_accumulate_and_repeat_skips_count(self):
        ready = self.prepare()
        samples = []
        with patch.object(core, 'ensure_game_closed'), patch.object(core, 'CHUNK', 16):
            self.swapper.install(ready, self.game, overall_progress=lambda *row: samples.append(row))
        total = sum(row['bytes'] for row in ready['archives'])
        self.assertEqual(samples[0][1], 0)
        self.assertEqual(samples[-1][1:], (total, total, len(ready['archives']), len(ready['archives'])))
        self.assertEqual([row[1] for row in samples], sorted(row[1] for row in samples))
        self.assertTrue(any(row[1] > 0 and row[3] == 0 for row in samples))
        skipped = []
        with patch.object(core, 'ensure_game_closed'), patch.object(fast, 'copy_archive', side_effect=AssertionError('repeat copied')):
            self.swapper.install(ready, self.game, overall_progress=lambda *row: skipped.append(row))
        self.assertEqual(skipped[-1][1:], samples[-1][1:])

    def test_failed_commit_does_not_count_archive_complete(self):
        ready = self.prepare()
        samples = []
        with patch.object(core, 'ensure_game_closed'), patch.object(fast, '_commit', side_effect=OSError('fixture commit failed')):
            with self.assertRaisesRegex(OSError, 'commit failed'):
                self.swapper.install(ready, self.game, overall_progress=lambda *row: samples.append(row))
        self.assertTrue(samples)
        self.assertTrue(all(row[3] == 0 for row in samples))


class ProgressUI(unittest.TestCase):
    setUp = ui_tests.DirectDesktop.setUp
    tearDown = ui_tests.DirectDesktop.tearDown

    def test_file_status_never_resets_overall_and_pending_commit_below100(self):
        self.app.busy = 'update'
        self.app.events.put(('overall-progress', 'Downloading b.vpk', 70, 100, 1, 2))
        self.app.events.put(('download-progress', 'Committing b.vpk', 0, 1))
        self.app.poll()
        self.assertEqual(float(self.app.progress.cget('value')), 70)
        self.assertIn('1/2 PAKs', self.app.overall_label.cget('text'))
        self.app.events.put(('overall-progress', 'Finishing', 100, 100, 2, 2))
        self.app.poll()
        self.assertLess(float(self.app.progress.cget('value')), 100)

    def test_install_uses_determinate_overall_and_success_finishes(self):
        self.app.busy = 'install'
        self.app.events.put(('overall-progress', 'Installing b.vpk', 35, 100, 1, 2))
        self.app.poll()
        self.assertEqual(str(self.app.progress.cget('mode')), 'determinate')
        self.assertIn('Overall install: 35.0%', self.app.overall_label.cget('text'))
        self.app.events.put(('done', 'install', {'status': 'installed', 'archives': []}))
        with patch.object(self.app, 'show_install_success') as notice:
            self.app.poll()
        notice.assert_called_once()
        self.assertEqual(float(self.app.progress.cget('value')), 100)
        self.assertIn('100%', self.app.overall_label.cget('text'))

    def test_worker_report_only_queues_and_pause_error_keeps_partial(self):
        worker = threading.Thread(target=lambda: self.app.report_overall('Downloaded', 12, 100, 0, 2))
        worker.start();worker.join()
        self.app.busy = 'update'
        self.app.poll()
        self.assertEqual(float(self.app.progress.cget('value')), 12)
        self.app.events.put(('error', 'update', OSError('fixture transfer failed')))
        self.app.poll()
        self.assertEqual(float(self.app.progress.cget('value')), 12)
        self.assertIsNone(self.app.busy)


if __name__ == '__main__':
    unittest.main()
