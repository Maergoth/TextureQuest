"""Pure, lossless-by-line ReShade shortcut sanitation.

Only ReShade configuration and preset shortcuts are covered. Add-ons can read
input themselves; their hardcoded hotkeys cannot be disabled by these settings.
No files are read or written here: callers own backup, CAS and installation.
"""
from __future__ import annotations

import re

MAX_INI_BYTES = 4 * 1024 * 1024
HOME = "36,0,0,0"
OFF = "0,0,0,0"
# Include the legacy performance-mode spelling and both load/save case variants
# of FrameTime through case-insensitive matching. Explicit local entries prevent
# ReShade's global-config fallback from restoring an absent shortcut.
INPUT_DEFAULTS = {
    "KeyOverlay": HOME,
    **{key: OFF for key in (
        "KeyEffects", "KeyScreenshot", "KeyNextPreset", "KeyPreviousPreset",
        "KeyReload", "KeyFPS", "KeyFrameTime", "KeyPerformanceMode",
    )},
}
GENERAL_DEFAULTS = {"PresetShortcutKeys": "", "PresetShortcutPaths": ""}

_SECTION = re.compile(r"^\s*\[([^]\r\n]+)\]\s*(?:[;#].*)?$")
_ASSIGNMENT = re.compile(r"^(\s*([^=;#\s][^=]*?)\s*=)(.*)$")
_QUAD = re.compile(r"^\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*$")


def _decode(raw: bytes) -> tuple[str, bytes]:
    if not isinstance(raw, bytes):
        raise TypeError("ReShade settings must be bytes.")
    if len(raw) > MAX_INI_BYTES:
        raise ValueError("ReShade settings exceed the 4 MiB limit.")
    bom = b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b""
    text = raw[len(bom):].decode("utf-8")
    if "\x00" in text:
        raise ValueError("ReShade settings contain a NUL byte.")
    return text, bom


def _ending(line: str) -> tuple[str, str]:
    for ending in ("\r\n", "\n", "\r"):
        if line.endswith(ending):
            return line[:-len(ending)], ending
    return line, ""


def _encode(text: str, bom: bytes) -> bytes:
    result = bom + text.encode("utf-8")
    if len(result) > MAX_INI_BYTES:
        raise ValueError("Sanitized ReShade settings exceed the 4 MiB limit.")
    return result


def _value_parts(value: str) -> tuple[str, str, str]:
    """Keep leading value spacing and any inline comment exactly."""
    comment = min((i for i in (value.find(";"), value.find("#")) if i >= 0), default=len(value))
    body, suffix = value[:comment], value[comment:]
    leading = body[:len(body) - len(body.lstrip())]
    trailing = body[len(body.rstrip()):] if body.strip() else ""
    return body.strip(), leading, trailing + suffix


def _replace(prefix: str, value: str, replacement: str) -> str:
    _, leading, suffix = _value_parts(value)
    # All-whitespace values must not duplicate their spacing on each pass.
    if not value.strip():
        leading, suffix = value, ""
    return prefix + leading + replacement + suffix


def sanitize_reshade_config(raw: bytes) -> bytes:
    """Leave Home as the sole ReShade built-in shortcut; preserve other settings.

    The independent TextureQuest shortcut file is never involved. Existing and
    unknown INPUT Key* entries are all disabled, including duplicate entries.
    """
    text, bom = _decode(raw)
    newline = "\r\n" if "\r\n" in text else "\n"
    defaults = {"input": INPUT_DEFAULTS, "general": GENERAL_DEFAULTS}
    seen = {name: set() for name in defaults}
    last_section: dict[str, int] = {}
    section = ""
    lines = text.splitlines(keepends=True)
    output = []
    for index, line in enumerate(lines):
        body, ending = _ending(line)
        header = _SECTION.fullmatch(body)
        if header:
            section = header[1].strip().casefold()
        elif body.lstrip().startswith("["):
            raise ValueError("Malformed ReShade settings section.")
        if section in defaults:
            last_section[section] = index
        match = _ASSIGNMENT.match(body)
        if match and section in defaults:
            key = match[2].strip().casefold()
            if section == "input" and key.startswith("key"):
                body = _replace(match[1], match[3], HOME if key == "keyoverlay" else OFF)
                seen[section].add(key)
            elif section == "general" and key in {k.casefold() for k in GENERAL_DEFAULTS}:
                body = _replace(match[1], match[3], "")
                seen[section].add(key)
        output.append(body + ending)

    # Insert missing explicit defaults at the end of the last matching section.
    # Work backwards so insertion positions from the original text stay valid.
    for name, index in sorted(last_section.items(), key=lambda item: item[1], reverse=True):
        missing = [key + "=" + value + newline for key, value in defaults[name].items()
                   if key.casefold() not in seen[name]]
        if missing:
            if output[index] and not output[index].endswith(("\r", "\n")):
                output[index] += newline
            output[index + 1:index + 1] = missing
    for name, values in defaults.items():
        if name in last_section:
            continue
        if output and not output[-1].endswith(("\r", "\n")):
            output[-1] += newline
        output.append("[" + name.upper() + "]" + newline)
        output.extend(key + "=" + value + newline for key, value in values.items())
    return _encode("".join(output), bom)


def sanitize_reshade_preset(raw: bytes) -> bytes:
    """Disable preset technique and uniform shortcuts without changing effects.

    ReShade 6.8 stores Key<Technique> and Key<Technique>@<effect.fx> in the
    unsectioned preamble, and Key<Uniform> under [<effect.fx>]. Only four-integer
    key tuples are rewritten, including noncanonical nonzero modifier values.
    A legitimate uniform
    named KeyFoo with an identical tuple is textually ambiguous with Foo's
    shortcut; callers needing that distinction must supply effect metadata.
    """
    text, bom = _decode(raw)
    section = ""
    output = []
    for line in text.splitlines(keepends=True):
        body, ending = _ending(line)
        header = _SECTION.fullmatch(body)
        if header:
            section = header[1].strip().casefold()
        elif body.lstrip().startswith("["):
            raise ValueError("Malformed ReShade preset section.")
        match = _ASSIGNMENT.match(body)
        if match and (not section or section.endswith(".fx")):
            key = match[2].strip()
            value, _, _ = _value_parts(match[3])
            quad = _QUAD.fullmatch(value)
            if (key.casefold().startswith("key") and len(key) > 3 and quad
                    and int(quad[1]) <= 255 and all(int(v) <= 0xffffffff for v in quad.groups())):
                body = _replace(match[1], match[3], OFF)
        output.append(body + ending)
    return _encode("".join(output), bom)
