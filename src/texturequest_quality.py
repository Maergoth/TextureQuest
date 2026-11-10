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


def fresh_values():
    """Appearance seed values, used only where no saved value exists."""
    return {(section, key): ('1' if value else '0') if type(value) is bool else str(value)
            for section, settings in contract()['fresh_defaults'].items()
            for key, value in settings.items()}


def description(name):
    if name == CUSTOM:
        return 'Custom performance settings. Presets change feature switches and budgets; your light strength, radius, colour and softness are kept.'
    row = preset(name)
    return row['target'] + ' class · ' + row.get('description', '') + ' Keeps your appearance settings.'


def _same(left, right):
    left = left.split(';', 1)[0].split('#', 1)[0].strip()
    try:
        a, b = float(left), float(right)
        return math.isfinite(a) and math.isfinite(b) and math.isclose(a, b, rel_tol=1e-5, abs_tol=1e-6)
    except ValueError:
        return left == right


SUN_ENABLE = ('EQ2Graphics', 'SunShadowSoftening')
SUN_KEYS = ('SunSoftness', 'SunShadowSize', 'TextureQuestSunSoftnessPreferenceRealSun',
            'TextureQuestSunSoftnessPreference', 'TextureQuestSunSoftnessUnits')


def _sun_settings(lines):
    """Mirror sun_softness_settings::Load; native values win without a marker.

    The shared performance contract owns an enable intent, not the softness
    magnitude. Legacy keys remain unchanged, as in the native Load/Store path.
    """
    rows, _ = _parse(lines, {('EQ2Graphics', key): None for key in SUN_KEYS}, casefold=True)
    if any(len(indices) > 1 for indices in rows.values()):
        raise ValueError('Duplicate sun settings; current settings were kept.')
    def read(key):
        indices = rows[('EQ2Graphics', key)]
        if not indices:
            return None
        raw = _assignment(lines[indices[0]])[2].split(';', 1)[0].split('#', 1)[0].strip()
        try:
            return float(raw)
        except ValueError as error:
            raise ValueError('Invalid sun softness; current settings were kept.') from error
    # Same definitions as graphics-renderer/sun_softness_settings.hpp.
    legacy_factor = (1.0 / 3.0) / 0.00465
    default, maximum = 1.6, 2.0 * legacy_factor
    def clean(value):
        return min(value, maximum) if math.isfinite(value) and value >= 0 else default
    modern, legacy = read('SunSoftness'), read('SunShadowSize')
    active = clean(modern) if modern is not None else clean(legacy * legacy_factor) if legacy is not None else 0.0
    preference = read('TextureQuestSunSoftnessPreferenceRealSun')
    old_preference = read('TextureQuestSunSoftnessPreference')
    if preference is not None:
        preference = clean(preference)
    elif old_preference is not None:
        preference = clean(old_preference * legacy_factor)
    else:
        preference = active if active > 0 else default
    return active, preference if preference > 0 else default


def detect(raw):
    """Match effective performance controls; appearance edits keep the tier."""
    lines, _ = _decode(raw)
    defaults = values('Low')
    rows, sections = _parse(lines, defaults, casefold=True)
    if any(len(indices) > 1 for indices in rows.values()):
        raise ValueError('Duplicate quality settings; current settings were kept.')
    # A genuinely absent world profile uses the renderer's fresh Low defaults.
    if not any(section.casefold() == 'eq2graphics' for _, section in sections):
        return 'Low'
    sun_enabled = _sun_settings(lines)[0] > 0
    for name in NAMES:
        expected = values(name)
        if sun_enabled != (expected.pop(SUN_ENABLE) == '1'):
            continue
        if all(rows[key] and _same(_assignment(lines[rows[key][0]])[2], expected[key])
               for key in expected):
            return name
    return CUSTOM


def apply(raw, name):
    """Replace performance switches/budgets, preserving appearance and comments."""
    expected = values(name)
    lines, bom = _decode(raw)
    enabled = expected.pop(SUN_ENABLE) == '1'
    active, preference = _sun_settings(lines)
    # Turning off remembers the actual active magnitude. Turning on restores it;
    # neither path chooses the preset author's artistic softness for the user.
    if active > 0 and not enabled:
        preference = active
    expected.update({('EQ2Graphics', 'SunSoftness'): str(active if active > 0 and enabled else preference if enabled else 0.0),
                     ('EQ2Graphics', 'TextureQuestSunSoftnessPreferenceRealSun'): str(preference),
                     ('EQ2Graphics', 'TextureQuestSunSoftnessUnits'): '1'})
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
