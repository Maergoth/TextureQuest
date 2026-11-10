"""Shared quality preset settings and explicit, per-game pending choices.

Reading this module or opening the launcher never changes game settings. The
normal graphics manager owns closed-game locks and journaled INI writes.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import sys

from texture_swapper import core
from texturequest_components import _game_identity
from texturequest_effects import _assignment, _decode, _encode, _ending, _replace_value

NAMES = ('Low', 'Medium', 'High', 'Max')
CUSTOM = 'Custom'
PENDING_SCHEMA = 'texturequest-quality-pending-v1'


def _parse(*args, **kwargs):
    from texturequest_dlss import _parse as parse
    return parse(*args, **kwargs)


def contract():
    path = Path(getattr(sys, '_MEIPASS', Path(__file__).parent)) / 'quality-presets.json'
    value = json.loads(path.read_text(encoding='utf-8'))
    if (value.get('schema') != 'texturequest-quality-presets-v1'
            or tuple(row['name'] for row in value['presets']) != NAMES):
        raise ValueError('The quality preset definitions are missing or unsupported.')
    return value


def preset(name):
    if name not in NAMES:
        raise ValueError('Choose Low, Medium, High or Max quality.')
    return next(row for row in contract()['presets'] if row['name'] == name)


def values(name):
    return {(section, key): ('1' if value else '0') if type(value) is bool else str(value)
            for section, settings in preset(name)['settings'].items()
            for key, value in settings.items()}


def description(name):
    if name == CUSTOM:
        return 'Custom in-game settings. Choose a preset to replace its lighting and shadow controls.'
    row = preset(name)
    return row['target'] + ' class · ' + row.get('description', '')


def _same(left, right):
    left = left.split(';', 1)[0].split('#', 1)[0].strip()
    try:
        a, b = float(left), float(right)
        return math.isfinite(a) and math.isfinite(b) and math.isclose(a, b, rel_tol=1e-5, abs_tol=1e-6)
    except ValueError:
        return left == right


def detect(raw):
    """Match effective managed controls, not a potentially stale preset label."""
    lines, _ = _decode(raw)
    defaults = values('Low')
    rows, sections = _parse(lines, defaults, casefold=True)
    if any(len(indices) > 1 for indices in rows.values()):
        raise ValueError('Duplicate quality settings; current settings were kept.')
    # A genuinely absent world profile uses the renderer's fresh Low defaults.
    if not any(section.casefold() == 'eq2graphics' for _, section in sections):
        return 'Low'
    for name in NAMES:
        expected = values(name)
        if all(rows[key] and _same(_assignment(lines[rows[key][0]])[2], expected[key])
               for key in expected):
            return name
    return CUSTOM


def apply(raw, name):
    """Replace only preset-owned keys, preserving other settings and comments."""
    expected = values(name)
    lines, bom = _decode(raw)
    rows, sections = _parse(lines, expected, casefold=True)
    if any(len(indices) > 1 for indices in rows.values()):
        raise ValueError('Duplicate quality settings; current settings were kept.')
    newline = next((_ending(line)[1] for line in lines if _ending(line)[1]), '\r\n')
    for key, value in expected.items():
        rows, sections = _parse(lines, expected, casefold=True)
        if rows[key]:
            index = rows[key][0]
            lines[index] = _replace_value(lines[index], value, scalar=True)
            continue
        starts = [index for index, section in sections if section == key[0]]
        if starts:
            insert = next((index for index, _ in sections if index > starts[-1]), len(lines))
            if insert and not _ending(lines[insert - 1])[1]:
                lines[insert - 1] += newline
            lines.insert(insert, key[1] + '=' + value + newline)
        else:
            if lines and not _ending(lines[-1])[1]:
                lines[-1] += newline
            lines.extend(['[' + key[0] + ']' + newline, key[1] + '=' + value + newline])
    return _encode(lines, bom)


def _path(state, game):
    identity = _game_identity(game)
    state = core._plain(state)
    base = core._plain(state / 'quality-pending', state)
    key = hashlib.sha256(identity.encode('utf-8')).hexdigest()
    return identity, core._plain(base / (key + '.json'), state), core._plain(base / (key + '.lock'), state)


def load_pending(state, game):
    identity, path, _ = _path(state, game)
    if not path.exists():
        return dict(choice=None, record_sha256=None)
    if path.stat().st_size > 4096:
        raise ValueError('Pending quality settings are too large; they were preserved.')
    record = core._load(path)
    if (record.get('schema') != PENDING_SCHEMA or record.get('game') != identity
            or record.get('choice') not in (*NAMES, None)):
        raise ValueError('Pending quality settings are unsupported; they were preserved.')
    return dict(choice=record['choice'], record_sha256=record['record_sha256'])


def save_pending(state, game, choice, *, expected):
    if choice not in (*NAMES, None):
        raise ValueError('Choose a supported quality preset.')
    identity, path, lock = _path(state, game)
    with core._lock(lock):
        current = load_pending(state, game)
        if current != expected:
            raise ValueError('Quality choice changed in another window. Reload before applying it.')
        if current['choice'] == choice:
            return current
        record = dict(schema=PENDING_SCHEMA, game=identity, choice=choice)
        core._save(path, record)
        return dict(choice=choice, record_sha256=record['record_sha256'])
