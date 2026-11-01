"""Pure, reversible settings preparation for the optional TextureQuest effect.

No I/O, runtime imports, effect activation, preset changes or installation occur.
Callers own closed-game locking, durable journals, CAS and shader-file ownership.
Keep the original journal on reinstall, or restore it before preparing a new one.

ReShade 6.8 source: source/ini_file.cpp splits string lists on single commas and
unescapes doubled commas; duplicate keys append values, scalar reads use the
first value. This helper refuses duplicate affected keys rather than guessing.
source/runtime.cpp skips initial effect loading with NoReloadOnInit=1 and may
skip our inactive technique with SkipLoadingDisabledEffects=1. Clearing both is
global loading work, not permission to enable or disable other effects.
"""
from __future__ import annotations

import hashlib
import json
import ntpath
import re

MAX_INI_BYTES = 4 * 1024 * 1024
SCHEMA = "texturequest-effects-settings-v1"
EFFECT_FILE = "TQEffects.fx"
TECHNIQUE = "TQBloom"
KEYS = ("EffectSearchPaths", "NoReloadOnInit", "SkipLoadingDisabledEffects")
_HEX = re.compile(r"[0-9a-f]{64}")
_SECTION = re.compile(r"^[ \t]*\[([^]\r\n]+)\].*$")


def _sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _seal(body: dict) -> dict:
    result = dict(body)
    result["record_sha256"] = _sha(json.dumps(body, sort_keys=True, ensure_ascii=False,
                                            separators=(",", ":"), allow_nan=False).encode("utf-8"))
    return result


def _decode(raw: bytes) -> tuple[list[str], bytes]:
    if not isinstance(raw, bytes):
        raise TypeError("ReShade settings must be bytes.")
    if len(raw) > MAX_INI_BYTES:
        raise ValueError("ReShade settings exceed the 4 MiB limit.")
    bom = b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b""
    text = raw[len(bom):].decode("utf-8")
    if "\x00" in text:
        raise ValueError("ReShade settings contain a NUL byte.")
    # str.splitlines also treats valid Unicode path characters (NEL/LS/PS) as
    # newlines. ReShade's byte-line reader does not split on those characters.
    return [line for line in re.findall(r"[^\r\n]*(?:\r\n|\r|\n|$)", text) if line], bom


def _encode(lines: list[str], bom: bytes) -> bytes:
    raw = bom + "".join(lines).encode("utf-8")
    if len(raw) > MAX_INI_BYTES:
        raise ValueError("Prepared ReShade settings exceed the 4 MiB limit.")
    return raw


def _ending(line: str) -> tuple[str, str]:
    for ending in ("\r\n", "\n", "\r"):
        if line.endswith(ending):
            return line[:-len(ending)], ending
    return line, ""


def _assignment(line: str):
    body, ending = _ending(line)
    stripped = body.strip(" \t")
    if not stripped or stripped[0] in ";/#":
        return None
    left, equals, value = body.partition("=")
    if not equals:
        return None
    return left.strip(" \t"), left + equals, value, ending


def _parse(lines: list[str]):
    rows = {key: [] for key in KEYS}
    sections = []
    section = ""
    for index, line in enumerate(lines):
        body, _ = _ending(line)
        stripped = body.strip(" \t")
        if stripped.startswith((";", "/", "#")):
            continue
        header = _SECTION.fullmatch(body)
        if header:
            section = header[1].strip(" \t[]")
            sections.append((index, section))
            continue
        if stripped.startswith("["):
            raise ValueError("Malformed ReShade settings section; settings preserved.")
        assignment = _assignment(line)
        if section == "GENERAL" and assignment and assignment[0] in rows:
            rows[assignment[0]].append(index)
    return rows, sections


def _split_paths(value: str) -> list[str]:
    if not value:
        return []
    result, current, index = [], [], 0
    while index < len(value):
        if value[index:index + 2] == ",,":
            current.append(",")
            index += 2
        elif value[index] == ",":
            result.append("".join(current))
            current = []
            index += 1
        else:
            current.append(value[index])
            index += 1
    result.append("".join(current))
    return result


def _directory(value: str) -> str:
    if (not isinstance(value, str) or not value or len(value) > 2048 or value != value.strip()
            or any(ord(c) < 32 or c in '<>"|?*' for c in value)):
        raise ValueError("Use one absolute local TextureQuest shader directory.")
    drive, tail = ntpath.splitdrive(value)
    if not re.fullmatch(r"[a-zA-Z]:", drive) or not tail.startswith(("/", "\\")) or ":" in tail:
        raise ValueError("Use one absolute local TextureQuest shader directory.")
    return ntpath.normpath(value)


def _path_identity(value: str) -> str:
    return ntpath.normcase(ntpath.normpath(value))


def _search_value(before: str | None) -> str:
    # The global ReShade.ini has an implicit current-directory search path.
    # An explicitly empty key instead supplies an empty vector and stays empty.
    return _assignment(before)[2].strip(" \t") if before is not None else ".\\"


def _replace_value(line: str, replacement: str, scalar: bool = False) -> str:
    _, prefix, value, ending = _assignment(line)
    # ReShade does not define inline comments in path lists; ;/# can be paths.
    # Preserve familiar scalar suffixes without treating them as extra settings.
    marker = min((i for i in (value.find(";"), value.find("#")) if i >= 0), default=len(value)) if scalar else len(value)
    body, suffix = value[:marker], value[marker:]
    leading = body[:len(body) - len(body.lstrip(" \t"))]
    trailing = body[len(body.rstrip(" \t")):] if body.strip(" \t") else ""
    return prefix + leading + replacement + trailing + suffix + ending


def prepare_effects_config(raw: bytes, shader_directory: str) -> tuple[bytes, dict]:
    """Prepare loading only; return bytes and a JSON-safe ownership journal.

    Existing effects/preset/paths are retained. This can load previously unloaded
    user effects because both loading restrictions must be cleared. The native
    master gate is still responsible for enabling only our exact named technique.
    Nothing returned here establishes that effects were installed or rendered.
    """
    directory = _directory(shader_directory)
    lines, bom = _decode(raw)
    original_tail = lines[-1] if lines else ""
    rows, sections = _parse(lines)
    if any(len(indices) > 1 for indices in rows.values()):
        raise ValueError("Duplicate affected ReShade settings; settings preserved.")
    newline = next((_ending(line)[1] for line in lines if _ending(line)[1]), "\r\n")
    changes, additions = [], []
    added_path = False
    for key in KEYS:
        before = lines[rows[key][0]] if rows[key] else None
        if key == "EffectSearchPaths":
            old_value = _search_value(before)
            paths = _split_paths(old_value)
            if any(_path_identity(path) == _path_identity(directory) for path in paths if path):
                continue  # A pre-existing path belongs to the user, never this journal.
            if any(not path for path in paths):
                raise ValueError("Empty effect-search list entries are ambiguous; settings preserved.")
            value = (old_value + "," if old_value else "") + directory.replace(",", ",,")
            added_path = True
        else:
            value = "0"
        after = _replace_value(before, value, scalar=key != "EffectSearchPaths") if before is not None else key + "=" + value + newline
        if after == before:
            continue
        changes.append({"key": key, "before": before, "after": after})
        if before is None:
            additions.append(after)
        else:
            lines[rows[key][0]] = after
    general_headers = [index for index, section in sections if section == "GENERAL"]
    created_general, tail_bridge = False, None
    if additions:
        if general_headers:
            last = general_headers[-1]
            position = next((index for index, _ in sections if index > last), len(lines))
        else:
            position, created_general = len(lines), True
        if position and not _ending(lines[position - 1])[1]:
            # A modified last target row will restore to its original body first.
            tail_bridge = {"line_sha256": _sha(original_tail.encode("utf-8")), "newline": newline}
            lines[position - 1] += newline
        block = (["[GENERAL]" + newline] if created_general else []) + additions
        lines[position:position] = block
    result = _encode(lines, bom)
    journal = _seal({"schema": SCHEMA, "effect_file": EFFECT_FILE, "technique": TECHNIQUE,
                     "shader_directory": directory, "requested": True, "installed": False,
                     "before_sha256": _sha(raw), "after_sha256": _sha(result), "changes": changes,
                     "path_added": added_path, "created_general": created_general, "tail_bridge": tail_bridge})
    _validate_journal(journal)
    return result, journal


def _validate_journal(journal: dict) -> None:
    keys = {"schema", "effect_file", "technique", "shader_directory", "requested", "installed",
            "before_sha256", "after_sha256", "changes", "path_added", "created_general", "tail_bridge", "record_sha256"}
    if not isinstance(journal, dict) or set(journal) != keys:
        raise ValueError("Invalid effects settings journal.")
    body = {key: value for key, value in journal.items() if key != "record_sha256"}
    try:
        sealed = _seal(body)
    except (TypeError, ValueError) as error:
        raise ValueError("Invalid effects settings journal.") from error
    if sealed != journal or journal["schema"] != SCHEMA or journal["effect_file"] != EFFECT_FILE or journal["technique"] != TECHNIQUE:
        raise ValueError("Effects settings journal identity/checksum mismatch.")
    if journal["requested"] is not True or journal["installed"] is not False:
        raise ValueError("Effects settings journal is not a preparation record.")
    if _directory(journal["shader_directory"]) != journal["shader_directory"]:
        raise ValueError("Invalid effects settings directory.")
    if any(not isinstance(journal[key], str) or not _HEX.fullmatch(journal[key]) for key in ("before_sha256", "after_sha256")):
        raise ValueError("Invalid effects settings file pin.")
    if type(journal["path_added"]) is not bool or type(journal["created_general"]) is not bool:
        raise ValueError("Invalid effects settings ownership flags.")
    changes = journal["changes"]
    if not isinstance(changes, list) or len(changes) > len(KEYS):
        raise ValueError("Invalid effects settings changes.")
    seen = set()
    for change in changes:
        if not isinstance(change, dict) or set(change) != {"key", "before", "after"} or change["key"] not in KEYS or change["key"] in seen:
            raise ValueError("Invalid effects settings change identity.")
        seen.add(change["key"])
        for field in ("before", "after"):
            line = change[field]
            if line is None and field == "before":
                continue
            if (not isinstance(line, str) or len(line.encode("utf-8")) > MAX_INI_BYTES or "\x00" in line
                    or any(char in _ending(line)[0] for char in "\r\n") or not _assignment(line)
                    or _assignment(line)[0] != change["key"]):
                raise ValueError("Invalid effects settings changed line.")
        before = change["before"]
        if change["key"] == "EffectSearchPaths":
            old_value = _search_value(before)
            expected = (old_value + "," if old_value else "") + journal["shader_directory"].replace(",", ",,")
        else:
            # Preserve the scalar's suffix exactly as preparation did.
            expected_line = _replace_value(before, "0", scalar=True) if before is not None else None
            expected = _assignment(expected_line)[2].strip(" \t") if expected_line else "0"
        if _assignment(change["after"])[2].strip(" \t") != expected:
            raise ValueError("Effects settings after-value is not owned.")
    if journal["path_added"] != ("EffectSearchPaths" in seen):
        raise ValueError("Effects search-path ownership mismatch.")
    if journal["created_general"] and (seen != set(KEYS) or any(change["before"] is not None for change in changes)):
        raise ValueError("Invalid effects settings section ownership.")
    bridge = journal["tail_bridge"]
    if bridge is not None and (not isinstance(bridge, dict) or set(bridge) != {"line_sha256", "newline"}
                               or not isinstance(bridge["line_sha256"], str) or not _HEX.fullmatch(bridge["line_sha256"])
                               or bridge["newline"] not in ("\r\n", "\n", "\r")):
        raise ValueError("Invalid effects settings newline ownership.")
    if bridge is not None and not any(change["before"] is None for change in changes):
        raise ValueError("Effects settings newline has no owned insertion.")


def restore_effects_config(raw: bytes, journal: dict) -> tuple[bytes, tuple[str, ...]]:
    """Restore only affected values still matching our after-state.

    Changed target values/duplicates remain untouched and are returned as conflict
    keys. Unrelated user edits, formatting, presets and comments are retained.
    The caller must retain the original journal while any conflicts remain.
    """
    _validate_journal(journal)
    lines, bom = _decode(raw)
    rows, _ = _parse(lines)
    replacements, conflicts = {}, []
    for change in journal["changes"]:
        key, before, after = change["key"], change["before"], change["after"]
        indices = rows[key]
        if not indices:
            if before is not None:
                conflicts.append(key)
            continue
        if len(indices) != 1:
            conflicts.append(key)
            continue
        index = indices[0]
        current_value = _assignment(lines[index])[2].strip(" \t")
        if current_value != _assignment(after)[2].strip(" \t"):
            if before is None or current_value != _assignment(before)[2].strip(" \t"):
                conflicts.append(key)
            continue
        if before is None:
            replacements[index] = ""
        elif lines[index] == after:
            replacements[index] = before
        else:
            replacements[index] = _replace_value(lines[index], _assignment(before)[2].strip(" \t"))
    lines = [replacements.get(index, line) for index, line in enumerate(lines) if replacements.get(index, line)]
    _, sections = _parse(lines)
    general = [index for index, name in sections if name == "GENERAL"]
    if journal["created_general"] and len(general) == 1:
        start = general[0]
        end = next((index for index, _ in sections if index > start), len(lines))
        if not any(line.strip() for line in lines[start + 1:end]):
            del lines[start]
    bridge = journal["tail_bridge"]
    if bridge and lines:
        body, ending = _ending(lines[-1])
        if ending == bridge["newline"] and _sha(body.encode("utf-8")) == bridge["line_sha256"]:
            lines[-1] = body
    return _encode(lines, bom), tuple(conflicts)
