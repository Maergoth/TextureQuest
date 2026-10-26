"""Explicit direct EQ2 launch; no patcher, installation or game-file changes."""
from pathlib import Path
import subprocess

from texture_swapper import core


def game_executable(game_folder) -> Path:
    """Return the validated game's direct client (a selected paks folder is normalized)."""
    folder = core._plain(core.validate_game(game_folder).parent)
    executable = next((path for path in folder.iterdir()
                       if path.name.casefold() == "everquest2.exe"), None)
    if executable is None:
        raise ValueError("The selected folder does not contain EverQuest2.exe")
    executable = core._plain(executable, folder)
    if executable.parent != folder or not executable.is_file():
        raise ValueError("The selected folder does not contain EverQuest2.exe")
    return executable


def launch_game(game_folder) -> subprocess.Popen:
    """Launch only after an explicit caller action; propagate validation/OS failures."""
    executable = game_executable(game_folder)
    core.ensure_game_closed()
    return subprocess.Popen([str(executable)], cwd=str(executable.parent), shell=False)
