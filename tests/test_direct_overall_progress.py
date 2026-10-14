"""Cumulative direct progress and truthful completion; own D fixtures only."""
import threading
import unittest
from unittest.mock import patch

import test_direct as td
import texturequest_direct as direct
import texturequest_release as release
import texturequest_fast_install as fast
from texture_swapper import core


class DirectOverallTests(unittest.TestCase):
    setUp = td.DirectTests.setUp
    opener = td.DirectTests.opener
    prepare = td.DirectTests.prepare
    legacy = td.DirectTests.legacy

    def events(self):
        result = []
        return result, lambda *event: result.append(event)

    def coherent(self, events):
        total = sum(row["bytes"] for row in self.manifest["archives"])
        self.assertTrue(events)
        self.assertEqual(events[0][1:], (0, total, 0, len(self.blobs)))
        self.assertTrue(all(event[2] == total and event[4] == len(self.blobs) for event in events))
        self.assertEqual([event[1] for event in events], sorted(event[1] for event in events))
        self.assertEqual([event[3] for event in events], sorted(event[3] for event in events))
        self.assertTrue(all(0 <= event[1] <= total and 0 <= event[3] <= len(self.blobs) for event in events))

    def test_streamed_bytes_accumulate_and_housekeeping_never_resets_them(self):
        events, report = self.events()
        legacy = []
        with patch.object(direct, "CHECKPOINT", 32):
            ready = self.prepare(overall_progress=report, progress=lambda *row: legacy.append(row))
        self.coherent(events)
        self.assertEqual(events[-1][1:], (ready["total_bytes"], ready["total_bytes"], 2, 2))
        first = self.manifest["archives"][0]["bytes"]
        second_stream = [row for row in events if row[0] == "Downloading b.vpk"]
        self.assertTrue(second_stream)
        self.assertEqual(second_stream[0][1], first + 32)
        commits = [row for row in events if row[0].startswith("Committing downloaded")]
        self.assertEqual([row[1] for row in commits], [first, ready["total_bytes"]])
        self.assertEqual([row[3] for row in commits], [0, 1])
        self.assertTrue(any(row[0].startswith("Downloading") and len(row) == 3 for row in legacy))

    def test_matching_cache_counts_complete_without_network(self):
        self.prepare()
        self.requests.clear()
        events, report = self.events()
        ready = self.prepare(overall_progress=report)
        self.coherent(events)
        self.assertEqual(self.requests, [])
        self.assertEqual(events[-1][1:], (ready["total_bytes"], ready["total_bytes"], 2, 2))

    def test_one_reused_file_and_one_download_have_one_total(self):
        folder = self.swapper.state_dir / "downloads"
        folder.mkdir()
        (folder / "a.vpk").write_bytes(self.blobs["a.vpk"])
        events, report = self.events()
        self.prepare(overall_progress=report)
        self.coherent(events)
        self.assertEqual([name for name, _ in self.requests], ["b.vpk"])
        self.assertTrue(any(row[3] == 1 and row[1] == len(self.blobs["a.vpk"]) for row in events))

    def test_paused_prefix_is_verified_and_resume_reports_its_bytes(self):
        pause = threading.Event()
        first_events, first_report = self.events()
        def legacy(message, *_):
            if message.startswith("Downloading"):
                pause.set()
        with patch.object(direct, "CHECKPOINT", 32), self.assertRaises(release.Paused):
            self.prepare(progress=legacy, pause=pause.is_set, overall_progress=first_report)
        self.assertFalse(any(row[3] == 2 for row in first_events))
        pause.clear()
        self.requests.clear()
        resumed, report = self.events()
        with patch.object(direct, "CHECKPOINT", 32):
            self.prepare(pause=pause.is_set, overall_progress=report)
        self.coherent(resumed)
        self.assertIn(("a.vpk", 32), self.requests)
        updates = [row for row in resumed if row[0] == "Downloading a.vpk"]
        self.assertEqual(updates[0][1], 64)
        self.assertEqual(resumed[-1][3], 2)

    def test_final_metadata_failure_cannot_report_all_files_complete(self):
        events, report = self.events()
        with patch.object(direct, "_retire_legacy_downloads", side_effect=OSError("fixture final receipt failure")):
            with self.assertRaisesRegex(OSError, "final receipt"):
                self.prepare(overall_progress=report)
        self.coherent(events)
        self.assertEqual(events[-1][1], events[-1][2])
        self.assertEqual(events[-1][3], 1)
        self.assertFalse(any(row[3] == 2 for row in events))

    def test_changed_prefix_preserved_and_no_complete_event(self):
        pause = threading.Event()
        with patch.object(direct, "CHECKPOINT", 32), self.assertRaises(release.Paused):
            self.prepare(progress=lambda message, *_: pause.set() if message.startswith("Downloading") else None,
                         pause=pause.is_set)
        partial = self.swapper.state_dir / "downloads/a.vpk.pending"
        original = bytearray(partial.read_bytes())
        original[0] ^= 1
        partial.write_bytes(original)
        pause.clear()
        events, report = self.events()
        with self.assertRaisesRegex(ValueError, "prefix changed"):
            self.prepare(pause=pause.is_set, overall_progress=report)
        self.assertEqual(partial.read_bytes(), original)
        self.assertFalse(any(row[3] for row in events))

    def test_migration_stage_is_status_only_then_moved_files_count_complete(self):
        self.legacy()
        events, report = self.events()
        self.prepare(overall_progress=report)
        self.coherent(events)
        moving = [row for row in events if row[0].startswith("Moving saved")]
        self.assertEqual(len(moving), 2)
        self.assertTrue(all(row[1] == row[3] == 0 for row in moving))
        self.assertEqual(self.requests, [])
        self.assertEqual(events[-1][3], 2)

    def test_direct_install_forwards_only_new_optional_callback(self):
        ready = self.prepare()
        report = lambda *_: None
        progress = lambda *_: None
        with patch.object(fast, "install", return_value={"status": "fixture"}) as install:
            self.swapper.install(ready, self.folder / "fixture-game", progress, overall_progress=report)
        install.assert_called_once_with(self.swapper, ready, self.folder / "fixture-game", progress,
                                       overall_progress=report)


if __name__ == "__main__":
    unittest.main()
