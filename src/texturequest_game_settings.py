"""Reversible, local-only EQ2 command INIs. Never edits eq2_default.ini.

The caller supplies an explicitly reviewed override map; this module invents no
native control or enum. Full originals stay only in portable data. One per-game
journal covers the selected feature set, not separate per-effect workflows.
"""
from contextlib import ExitStack
import hashlib
import os
from pathlib import Path
import re
import uuid

from texture_swapper import core
from texturequest_custody import DirectoryCustody, FileCustody
from texturequest_processes import ensure_closed

SCHEMA = "texturequest-native-settings-v1"
FILES = frozenset({"eq2.ini", "eq2_recent.ini"})
MAX_INI = 4 * 1024 * 1024
KEY = re.compile(r"r_[a-z0-9_]{1,80}", re.ASCII)
VALUE = re.compile(r"[A-Za-z0-9_.+-]{1,64}", re.ASCII)
COMMAND = re.compile(r"^([ \t]*)(r_[A-Za-z0-9_]+)([ \t]*=[ \t]*|[ \t]+)([^ \t\r\n;#]*?)([ \t]*(?:(?:;|#|//).*)?)(\r\n|\n|\r|)$", re.ASCII)

# No verified native disable controls were established by the permitted local
# UI/default-option evidence. These are visible policy gaps, not working options.
UNMANAGED = {
    "ambient_occlusion": "No verified native AO disable control; baked AO cannot be toggled by an INI guess.",
    "contact_shadows": "Supplement only; retain native world shadows and base lighting.",
    "indirect_lighting": "Screen-space supplement; retain native base lights/materials.",
    "bloom": "No verified native bloom disable key/value in the inspected client UI.",
    "anti_aliasing": "No verified native AA disable key/value in the inspected client UI.",
    "depth_diagnostic": "Diagnostic only; no native setting replacement.",
}


def _hash(raw): return {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def _decode(raw):
    if raw.startswith(b"\xff\xfe"): return raw[2:].decode("utf-16-le"), "utf-16-le", b"\xff\xfe"
    if raw.startswith(b"\xfe\xff"): return raw[2:].decode("utf-16-be"), "utf-16-be", b"\xfe\xff"
    if raw.startswith(b"\xef\xbb\xbf"): return raw[3:].decode("utf-8", "surrogateescape"), "utf-8", b"\xef\xbb\xbf"
    return raw.decode("latin-1"), "latin-1", b""


def _encode(text, encoding, bom): return bom + text.encode(encoding, "surrogateescape")


def _edit(raw, changes):
    text, encoding, bom = _decode(raw)
    lines = text.splitlines(keepends=True)
    newline = next((match.group(0) for match in re.finditer(r"\r\n|\n|\r", text)), "\r\n")
    owned = {key: {"before": [], "after": []} for key in changes}
    for index, line in enumerate(lines):
        match = COMMAND.fullmatch(line)
        key = match.group(2).lower() if match else None
        if key not in changes: continue
        if len(owned[key]["before"]) >= 64 or len(line) > 4096:
            raise ValueError("Managed INI occurrence limit exceeded")
        owned[key]["before"].append(line)
        replacement = "".join((match.group(1), match.group(2), match.group(3), changes[key], match.group(5), match.group(6)))
        lines[index] = replacement
        owned[key]["after"].append(replacement)
    added_separator = False
    for key, row in owned.items():
        if row["before"]: continue
        if lines and not lines[-1].endswith(("\n", "\r")):
            last_match = COMMAND.fullmatch(lines[-1])
            last_key = last_match.group(2).lower() if last_match else None
            lines[-1] += newline; added_separator = True
            if last_key in owned and owned[last_key]["after"]:
                owned[last_key]["after"][-1] = lines[-1]
        line = key + " " + changes[key] + newline
        lines.append(line); row["after"].append(line)
    result = _encode("".join(lines), encoding, bom)
    if len(result) > MAX_INI: raise ValueError("Modified INI exceeds the file limit")
    return result, owned, added_separator


def _merge(current, original, owned, added_separator):
    text, encoding, bom = _decode(current)
    lines = text.splitlines(keepends=True)
    found = {key: [] for key in owned}
    for index, line in enumerate(lines):
        match = COMMAND.fullmatch(line)
        key = match.group(2).lower() if match else None
        if key in found: found[key].append(index)
    conflicts, replacements = [], {}
    for key, row in owned.items():
        positions = found[key]
        if len(positions) != len(row["after"]):
            conflicts.append(dict(key=key, reason="occurrence_count_changed")); continue
        for ordinal, index in enumerate(positions):
            if lines[index] != row["after"][ordinal]:
                conflicts.append(dict(key=key, reason="owned_line_changed")); continue
            replacement = row["before"][ordinal] if row["before"] else ""
            if replacement and not replacement.endswith(("\r", "\n")):
                ending = re.search(r"\r\n|\r|\n", lines[index])
                if ending: replacement += ending.group(0)
            replacements[index] = replacement
    merged = [replacements.get(index, line) for index, line in enumerate(lines)]
    merged = [line for line in merged if line]
    # Undo only our separator at an otherwise unchanged original unterminated
    # final line, and only if no later user line needs that separator.
    if added_separator and merged:
        before_text, _, _ = _decode(original)
        before_lines = before_text.splitlines(keepends=True)
        if before_lines and not before_lines[-1].endswith(("\n", "\r")):
            last = merged[-1]
            if last.rstrip("\r\n") == before_lines[-1]: merged[-1] = before_lines[-1]
    return _encode("".join(merged), encoding, bom), conflicts


def _held_snapshot(held):
    held.stream.seek(0); raw = held.stream.read(MAX_INI + 1)
    if len(raw) > MAX_INI: raise ValueError("EQ2 INI exceeds the bounded editor limit")
    return raw, dict(exists=True, **_hash(raw), identity=held.identity())


def _read(path):
    path = core._plain(path)
    if not path.exists(): return b"", dict(exists=False, **_hash(b""), identity=None)
    with FileCustody(path) as held: return _held_snapshot(held)


def _commit(path, expected, raw, token):
    """CAS via held file identities and non-overwriting native renames.

    Two renames briefly retire the old path while EQ2 is closed. Foreign target
    bytes are never replaced. A synchronous failure restores the held old file;
    an abrupt interruption preserves the named temps for explicit recovery.
    """
    path = core._plain(path)
    new_path = core._plain(path.with_name("." + path.name + ".tq-" + token + ".new"))
    old_path = core._plain(path.with_name("." + path.name + ".tq-" + token + ".old"))
    if new_path.exists() or old_path.exists(): raise ValueError("Interrupted owned INI files were preserved")
    with DirectoryCustody(path.parent) as directories, ExitStack() as stack:
        old = stack.enter_context(FileCustody(path)) if expected["exists"] else None
        actual = _held_snapshot(old)[1] if old else _read(path)[1]
        if actual != expected: raise ValueError("EQ2 INI changed before the atomic handoff")
        new = None
        if raw is not None:
            with new_path.open("xb") as stream:
                stream.write(raw); stream.flush(); os.fsync(stream.fileno())
            new = stack.enter_context(FileCustody(new_path))
            if _held_snapshot(new)[1] != dict(exists=True, **_hash(raw), identity=new.identity()):
                raise ValueError("Staged INI changed")
        retired = False
        try:
            if old:
                old.rename_new(old_path, directories); retired = True
            if new: new.rename_new(path, directories)
        except Exception:
            if retired and not path.exists(): old.rename_new(path, directories)
            if new and new.path == new_path: new.remove()
            raise
        if old: old.remove()
        return _held_snapshot(new)[1] if new else dict(exists=False, **_hash(b""), identity=None)


class GameSettings:
    def __init__(self, state_dir, *, closed=None):
        self.state = core._plain(state_dir)
        self.base = core._plain(self.state / "native-settings", self.state)
        self.closed = closed or ensure_closed

    def _home(self, game):
        game = core._plain(game)
        if not game.is_dir(): raise ValueError("EQ2 folder is missing")
        key = hashlib.sha256(str(game).casefold().encode()).hexdigest()[:24]
        return game, core._plain(self.base / key, self.state)

    def _record(self, game, home):
        path = home / "current.json"
        if not path.exists(): return None
        value = core._load(path)
        if value.get("schema") != SCHEMA or value.get("game") != str(game):
            raise ValueError("Native-settings record changed; preserve its backups")
        if value.get("phase") not in {"applying", "applied", "restoring", "restored", "restored_with_conflicts"}:
            raise ValueError("Unknown native-settings transaction phase")
        if not re.fullmatch(r"[a-f0-9]{32}", value.get("id", "")):
            raise ValueError("Native-settings action identity changed")
        for name, row in value.get("files", {}).items():
            if name not in FILES or row.get("backup") != "backups/" + value["id"] + "/" + name:
                raise ValueError("Native-settings backup scope changed")
        return value

    def status(self, game):
        game, home = self._home(game); value = self._record(game, home)
        if value is None: return dict(status="unmanaged", files=[], conflicts=[], unmanaged=dict(UNMANAGED))
        return dict(status=value["phase"], files=sorted(value["files"]), features=value["features"],
                    conflicts=value.get("conflicts", []), unmanaged=dict(UNMANAGED), journal=str(home / "current.json"))

    def apply(self, game, overrides, *, features=()):
        """Apply only a caller-reviewed filename -> r_command/value map."""
        if not isinstance(overrides, dict) or len(overrides) > 2: raise ValueError("Invalid INI override map")
        normalized = {}
        for name, changes in overrides.items():
            if name not in FILES or not isinstance(changes, dict) or len(changes) > 64:
                raise ValueError("Only reviewed eq2.ini/eq2_recent.ini commands may be managed")
            normalized[name] = {}
            for key, val in changes.items():
                if not isinstance(key, str) or not KEY.fullmatch(key) or not isinstance(val, str) or not VALUE.fullmatch(val):
                    raise ValueError("A managed graphics command/value is invalid")
                normalized[name][key] = val
        normalized = {name: rows for name, rows in normalized.items() if rows}
        game, home = self._home(game)
        if not normalized: return self.status(game)
        if not isinstance(features, (tuple, list)) or len(features) > 16 or not all(isinstance(feature, str) and re.fullmatch(r"[a-z_]{1,64}", feature) for feature in features):
            raise ValueError("Invalid feature labels")
        with core._lock(self.state / "operation.lock"), core._lock(self.state / "sideload/helper-owner.lock"):
            self.closed()
            existing = self._record(game, home)
            if existing and existing["phase"] not in {"restored", "restored_with_conflicts"}:
                if existing["phase"] == "applied" and existing["overrides"] == normalized:
                    if all(_read(game / name)[1] == row["after"] for name, row in existing["files"].items()):
                        return self.status(game)
                raise ValueError("Restore the previous native-settings transaction before changing it")
            home.mkdir(parents=True, exist_ok=True)
            token = uuid.uuid4().hex; backup = home / "backups" / token; backup.mkdir(parents=True)
            value = dict(schema=SCHEMA, id=token, game=str(game), phase="applying", features=list(features), overrides=normalized, files={}, conflicts=[])
            plans = {}
            for name, changes in normalized.items():
                before_raw, before = _read(game / name)
                after_raw, owned, separator = _edit(before_raw, changes)
                with (backup / name).open("xb") as stream:
                    stream.write(before_raw); stream.flush(); os.fsync(stream.fileno())
                backup_raw, _ = _read(backup / name)
                if backup_raw != before_raw: raise ValueError("Full INI backup did not verify")
                value["files"][name] = dict(before=before, planned_after=_hash(after_raw), backup="backups/" + token + "/" + name, owned=owned, added_separator=separator)
                plans[name] = after_raw
            core._save(home / "current.json", value)
            for name, raw in plans.items():
                self.closed()
                row = value["files"][name]
                row["after"] = _commit(game / name, row["before"], raw, token)
                core._save(home / "current.json", value)
            value["phase"] = "applied"; core._save(home / "current.json", value)
            core._save(home / ("record-" + token + ".json"), value)
            return self.status(game)

    def restore(self, game):
        game, home = self._home(game)
        with core._lock(self.state / "operation.lock"), core._lock(self.state / "sideload/helper-owner.lock"):
            self.closed(); value = self._record(game, home)
            if value is None or value["phase"] in {"restored", "restored_with_conflicts"}: return self.status(game)
            plans = {}
            conflicts = list(value.get("conflicts", [])) if value["phase"] == "restoring" else []
            restores = value.setdefault("restore_steps", {})
            for name, row in value["files"].items():
                if any((game / ("." + name + ".tq-" + value["id"] + suffix)).exists() for suffix in (".new", ".old")):
                    raise ValueError("Interrupted owned INI files need explicit recovery; backups were preserved")
                original, backup_pin = _read(home / row["backup"])
                if {key: backup_pin[key] for key in ("bytes", "sha256")} != {key: row["before"][key] for key in ("bytes", "sha256")}:
                    raise ValueError("Full INI backup changed; original and current files were preserved")
                current, snapshot = _read(game / name)
                actual_hash = {key: snapshot[key] for key in ("bytes", "sha256")}
                before_hash = {key: row["before"][key] for key in ("bytes", "sha256")}
                step = restores.get(name) if value["phase"] == "restoring" else None
                if step is not None:
                    if "after" in step and snapshot == step["after"]:
                        continue
                    # A successful native commit may precede its journal ACK.
                    if "after" not in step and dict(exists=snapshot["exists"], **actual_hash) == step["planned_after"]:
                        step["after"] = snapshot
                        continue
                    if snapshot != step["before"]:
                        raise ValueError("Interrupted INI restore has changed bytes or identity; explicit recovery is required")
                if actual_hash == before_hash and snapshot["exists"] == row["before"]["exists"]:
                    continue
                if actual_hash == row["planned_after"] and snapshot["exists"]:
                    restored = original if row["before"]["exists"] else None
                elif value["phase"] != "applied" and step is None:
                    raise ValueError("Interrupted INI transaction has changed bytes; explicit recovery is required")
                elif not snapshot["exists"]:
                    conflicts.append(dict(file=name, reason="file_missing")); continue
                else:
                    restored, found = _merge(current, original, row["owned"], row["added_separator"])
                    for item in found:
                        conflict = dict(file=name, **item)
                        if conflict not in conflicts: conflicts.append(conflict)
                if step is not None and dict(exists=restored is not None, **_hash(restored or b"")) != step["planned_after"]:
                    raise ValueError("Interrupted INI restore plan changed; backups were preserved")
                plans[name] = (snapshot, restored)
            for name, (snapshot, raw) in plans.items():
                if name not in restores:
                    restores[name] = dict(before=snapshot, planned_after=dict(exists=raw is not None, **_hash(raw or b"")))
            value["conflicts"] = conflicts
            value["phase"] = "restoring"; core._save(home / "current.json", value)
            for name, (snapshot, raw) in plans.items():
                self.closed()
                restores[name]["after"] = _commit(game / name, snapshot, raw, uuid.uuid4().hex)
                core._save(home / "current.json", value)
            value["conflicts"] = conflicts
            value["phase"] = "restored_with_conflicts" if conflicts else "restored"
            core._save(home / "current.json", value)
            core._save(home / ("record-" + value["id"] + ".json"), value)
            return self.status(game)
