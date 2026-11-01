"""Typed optional-component preferences, separate from installed capabilities.

Preferences never establish that a component is installed or runtime-ready.
Only TextureQuest's normal per-game lock coordinates writes; arbitrary external
editors bypassing that lock are not covered by the application CAS protocol.
The game path is normalized as text and is never opened or inspected.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re

from texture_swapper import core

SCHEMA = "texturequest-components-v1"
RECORD_SCHEMA = "texturequest-component-preferences-v1"
MAX_RECORD_BYTES = 4096
_SELECTION_KEYS = {"schema", "textures", "dlss", "effects"}
_RECORD_KEYS = {"schema", "game", "selection", "revision", "updated_at", "record_sha256"}
_SNAPSHOT_KEYS = {"selection", "record_sha256"}
_SHA = re.compile(r"[a-f0-9]{64}")


def normalize(value=None):
    """Return a strict fresh selection; DLSS remains explicitly unavailable."""
    if value is None:
        return dict(schema=SCHEMA, textures=True, dlss=False, effects=False)
    if type(value) is not dict or set(value) != _SELECTION_KEYS:
        raise ValueError("Unknown TextureQuest component selection fields.")
    if value["schema"] != SCHEMA:
        raise ValueError("Unknown TextureQuest component selection schema.")
    if any(type(value[name]) is not bool for name in ("textures", "dlss", "effects")):
        raise ValueError("TextureQuest component choices must be booleans.")
    if not value["textures"]:
        raise ValueError("Textures are required.")
    if value["dlss"]:
        raise ValueError("DLSS is not available yet; it cannot be selected.")
    return {key: value[key] for key in ("schema", "textures", "dlss", "effects")}


def _game_identity(game):
    try:
        value = os.fspath(game)
    except TypeError as error:
        raise ValueError("Choose a game folder before saving components.") from error
    if type(value) is not str or not value.strip() or "\x00" in value or len(value) > 32767:
        raise ValueError("Invalid game folder identity.")
    if not os.path.isabs(value):
        raise ValueError("Component preferences require an absolute game folder.")
    # abspath/normcase operate on the spelling only: no resolve/stat/read of game.
    identity = os.path.normcase(os.path.abspath(value))
    if len(identity.encode("utf-8")) > 2048:
        raise ValueError("Game folder identity is too long for component preferences.")
    return identity


def _paths(state_dir, game):
    identity = _game_identity(game)
    state = core._plain(state_dir)
    base = core._plain(state / "components", state)
    key = hashlib.sha256(identity.encode("utf-8")).hexdigest()
    return identity, core._plain(base / (key + ".json"), state), core._plain(base / (key + ".lock"), state)


def _snapshot(record):
    if record is None:
        return dict(selection=normalize(), record_sha256=None)
    return dict(selection=normalize(record["selection"]), record_sha256=record["record_sha256"])


def _read(path, identity):
    if not path.exists():
        return None
    if path.stat().st_size > MAX_RECORD_BYTES:
        raise ValueError("Component preferences are too large; preserve them for recovery.")
    try:
        record = core._load(path)
    except (AttributeError, TypeError) as error:
        raise ValueError("Malformed component preferences; preserve them for recovery.") from error
    if (type(record) is not dict or set(record) != _RECORD_KEYS or record["schema"] != RECORD_SCHEMA
            or record["game"] != identity or type(record["revision"]) is not int
            or not 1 <= record["revision"] <= 2**63 - 1
            or type(record["updated_at"]) is not str or not 1 <= len(record["updated_at"]) <= 64
            or type(record["record_sha256"]) is not str or not _SHA.fullmatch(record["record_sha256"])
            or type(record["selection"]) is not dict):
        raise ValueError("Component preferences changed or are unsupported; preserve them for recovery.")
    normalize(record["selection"])
    return record


def _expected(value):
    if value is None:
        return _snapshot(None)
    if (type(value) is not dict or set(value) != _SNAPSHOT_KEYS
            or type(value["selection"]) is not dict):
        raise ValueError("A component preference snapshot is required.")
    selection = normalize(value["selection"])
    checksum = value["record_sha256"]
    if checksum is not None and (type(checksum) is not str or not _SHA.fullmatch(checksum)):
        raise ValueError("Invalid component preference snapshot checksum.")
    if checksum is None and selection != normalize():
        raise ValueError("An absent component record must have default choices.")
    return dict(selection=selection, record_sha256=checksum)


def load(state_dir, game):
    """Return {selection, record_sha256}; absence defaults to textures only."""
    identity, path, _ = _paths(state_dir, game)
    return _snapshot(_read(path, identity))


def save(state_dir, game, value, *, expected_before):
    """Save under normal lock/CAS; expected_before=None requires record absence.

    Pass the complete snapshot returned by load. A stale or malformed existing
    record refuses without overwriting it. Same-current-choice saves are no-ops.
    """
    selection = normalize(value)
    expected = _expected(expected_before)
    identity, path, lock = _paths(state_dir, game)
    with core._lock(lock):
        current = _read(path, identity)
        if _snapshot(current) != expected:
            raise ValueError("Component choices changed in another window. Reload them before saving.")
        if current is not None and current["selection"] == selection:
            return _snapshot(current)
        revision = 1 if current is None else current["revision"] + 1
        if revision > 2**63 - 1:
            raise ValueError("Component preference revision limit reached; preserve the record.")
        record = dict(schema=RECORD_SCHEMA, game=identity, selection=selection, revision=revision)
        core._save(path, record)
        return _snapshot(record)
