"""New direct-launch tests: D-only fixtures and mocked process creation."""
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_launch as launch
from texture_swapper import core


class LaunchGameTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="launch-game-", dir=Path(__file__).resolve().parent)
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name) / "EverQuest II fixture with spaces"
        self.paks = self.folder / "paks"
        self.paks.mkdir(parents=True)
        self.exe = self.folder / "EverQuest2.exe"
        self.exe.write_bytes(b"Own non-executable fixture marker; never launched")
        self.patcher = self.folder / "LaunchPad.exe"
        self.patcher.write_bytes(b"Own fixture; never launched")

    def test_game_executable_is_the_selected_direct_client(self):
        with patch.object(launch.subprocess, "Popen") as popen:
            self.assertEqual(launch.game_executable(str(self.folder)), self.exe)
        popen.assert_not_called()

    def test_launch_uses_one_argv_executable_correct_cwd_and_no_shell(self):
        before = {p.relative_to(self.folder):p.read_bytes() for p in self.folder.rglob("*") if p.is_file()}
        process = object()
        with patch.object(core, "ensure_game_closed") as guard, patch.object(launch.subprocess, "Popen", return_value=process) as popen:
            self.assertIs(launch.launch_game(self.folder), process)
        guard.assert_called_once_with()
        popen.assert_called_once_with([str(self.exe)], cwd=str(self.folder), shell=False)
        self.assertEqual(before, {p.relative_to(self.folder):p.read_bytes() for p in self.folder.rglob("*") if p.is_file()})

    def test_missing_client_does_not_fall_back_to_official_patcher(self):
        self.exe.unlink()
        with patch.object(core, "ensure_game_closed") as guard, patch.object(launch.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(ValueError, "EverQuest2.exe"):
                launch.launch_game(self.folder)
        guard.assert_not_called()
        popen.assert_not_called()

    def test_wrong_folder_without_paks_refuses(self):
        self.paks.rmdir()
        with patch.object(launch.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(ValueError, "paks"):
                launch.launch_game(self.folder)
        popen.assert_not_called()

    def test_unrelated_subdirectory_is_not_searched_or_launched(self):
        child = self.folder / "unrelated-subdirectory"
        child.mkdir()
        with patch.object(launch.subprocess, "Popen") as popen:
            with self.assertRaises(ValueError):
                launch.launch_game(child)
        popen.assert_not_called()

    def test_selected_paks_directory_normalizes_to_validated_game_root(self):
        with patch.object(core, "ensure_game_closed"), patch.object(launch.subprocess, "Popen") as popen:
            launch.launch_game(self.paks)
        popen.assert_called_once_with([str(self.exe)], cwd=str(self.folder), shell=False)

    def test_hardlinked_client_is_rejected_by_existing_path_guard(self):
        os.link(self.exe, Path(self.temporary.name) / "other-name.exe")
        with patch.object(launch.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(ValueError, "Hard-linked"):
                launch.launch_game(self.folder)
        popen.assert_not_called()

    def test_reported_client_reparse_attribute_is_rejected(self):
        original_lstat = Path.lstat
        def reparse(path, *args, **kwargs):
            info = original_lstat(path, *args, **kwargs)
            if path == self.exe:
                return SimpleNamespace(st_mode=info.st_mode, st_nlink=info.st_nlink,
                                       st_file_attributes=getattr(info, "st_file_attributes", 0) | 0x400)
            return info
        with patch.object(Path, "lstat", reparse), patch.object(launch.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(ValueError, "junctions"):
                launch.launch_game(self.folder)
        popen.assert_not_called()

    def test_running_client_or_patcher_refusal_precedes_process_creation(self):
        with patch.object(core, "ensure_game_closed", side_effect=RuntimeError("Close EQ2 first")), patch.object(launch.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(RuntimeError, "Close EQ2"):
                launch.launch_game(self.folder)
        popen.assert_not_called()

    def test_OS_launch_failure_propagates_to_the_caller(self):
        error = OSError("invented OS launch refusal")
        with patch.object(core, "ensure_game_closed"), patch.object(launch.subprocess, "Popen", side_effect=error):
            with self.assertRaises(OSError) as result:
                launch.launch_game(self.folder)
        self.assertIs(result.exception, error)


if __name__ == "__main__":
    unittest.main(verbosity=2)
