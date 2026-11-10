"""Pure, narrow settings leases for the two private DLSS profiles.

No files, DLLs, presets, technique lists or hotkeys are opened here. The caller
owns closed-game locking and persists the journal before applying these bytes.
Only the selected backend's exact settings change; backends are not mixed.
"""
from texturequest_effects import (
    _assignment, _decode, _encode, _ending, _replace_value, _seal, _sha,
    _split_paths, _SECTION, MAX_INI_BYTES,
)

SCHEMA = 'texturequest-dlss-settings-v1'
AIO_SCHEMA = 'texturequest-dlss-aio-settings-v1'
AIO_V2_SCHEMA = 'texturequest-dlss-aio-settings-v2'
AIO_V3_SCHEMA = 'texturequest-dlss-aio-settings-v3'
MACRO = 'DLSS5_MV_PROVIDER'
VALUES = {
    ('GENERAL', 'PreprocessorDefinitions'): None,
    ('RenoDX.DLSS5', 'EnableHooks'): '2',
    ('RenoDX.DLSS5', 'NeuralUplift'): '1',
    ('RenoDX.DLSS5', 'NREnableUpscaling'): '0',
    ('RenoDX.DLSS5', 'NRToggleKey'): '0',
    ('RenoDX.DLSS5', 'NRScreenshotKey'): '0',
    ('RenoDX.DLSS5', 'NRShotKey'): '0',
    ('RenoDX.DLSS5', 'NRStyle'): '0',
    ('RenoDX.DLSS5', 'NRPasses'): '1',
}
AIO_VALUES = {('Standalone.DLSSNR', key): value for key, value in {
    'Enabled': '1', 'NeuralRendering': '1', 'FrameGeneration': '1',
    'NvidiaOpticalFlowMotion': '1', 'NvidiaOpticalFlowResolution': '0', 'VortGuides': '0',
    'SourceResolutionOverride': '0', 'AutoWindowedVirtualization': '0',
    'WindowedVirtualization': '0', 'WindowedLogicalSizeMessages': '0',
    'WindowedInputScaling': '0', 'DpiPhysicalOutputCorrection': '0',
    'DetachedPresentation': '0', 'HideDetachedSystemCursor': '0',
    'EarlyProxyInitialization': '0', 'OpaqueComposition': '0',
    'AdaptivePressureGovernor': '0', 'ShowProxyFps': '0',
}.items()}
# Old journals retain their original owned defaults. The merged launcher option
# only installs current neural capabilities; absent feature choices start off.
AIO2_VALUES = dict(AIO_VALUES)
AIO2_VALUES.update({('Standalone.DLSSNR', key): '0'
                    for key in ('NeuralRendering', 'FrameGeneration', 'ManagedSrEnabled')})
AIO_CONTROL_KEYS = {('Standalone.DLSSNR', key) for key in
                    ('NeuralRendering', 'FrameGeneration', 'ManagedSrEnabled', 'SourceResolutionOverride')}

# Retain the exact v1 lease authority so an older install can still restore its
# owned settings. New installs use v2 defaults below; saved choices stay intact.
LEGACY_GRAPHICS_CORE_DEFAULTS = {('TextureQuest', 'ReflexLowLatency'): '1'}
LEGACY_GRAPHICS_WORLD_DEFAULTS = {('EQ2Graphics', key): value for key, value in {
    'AmbientKept': '0.5', 'AmbientOcclusion': '1', 'AnisotropicFiltering': '1',
    'Anisotropy': '16', 'CharacterShadows': '1', 'EmissiveLights': '1',
    'EmissiveLightStrength': '15.59', 'OcclusionRadius': '0.66', 'OcclusionStrength': '1.25',
    'OwnLampShaders': '0', 'PaintedEmissionKept': '0.5', 'PaintedEmissionLight': '0.5',
    'PointLightShadows': '1', 'ShadowedLights': '256',
    'PointShadowSoftness': '1.6', 'ShadowStrength': '1', 'SmoothIndoorTransition': '1',
    'SmoothInterfaceText': '1', 'SunLighting': '1', 'SunShadowDirectionFix': '0',
    'SunShadowMode': '2', 'SunSoftness': '1.6', 'TextureQuestSunSoftnessPreferenceRealSun': '1.6',
    'TextureQuestPresetVersion': '2',
}.items()}
GRAPHICS_AIO_DEFAULTS = {('Standalone.DLSSNR', key): value for key, value in {
    'FrameGeneration': '0', 'ManagedSrEnabled': '0', 'NeuralRendering': '0',
    'ManagedFrameMultiplier': '2', 'Model': '3', 'ManagedSrMode': '1', 'ManagedNrPasses': '1',
}.items()}
GRAPHICS_CORE_DEFAULTS = {}
GRAPHICS_WORLD_DEFAULTS = dict(LEGACY_GRAPHICS_WORLD_DEFAULTS)
GRAPHICS_WORLD_DEFAULTS.update({('EQ2Graphics', key): value for key, value in {
    'PointLightShadows': '0', 'EmissiveLights': '0', 'SmoothInterfaceText': '0',
    'AmbientOcclusion': '1', 'SunLighting': '0', 'SunSoftness': '0',
    'ShadowedLights': '50', 'ParticleShadowLimit': '50',
    'EmissiveLightLimit': '64', 'ParticleLightLimit': '64',
}.items()})
_GRAPHICS_PROFILE_NAMES = ('graphics-core-defaults', 'graphics-world-defaults',
                           'graphics-aio-defaults', 'graphics-world-aio-defaults')
LEGACY_GRAPHICS_DEFAULT_PROFILES = {
    name: ('texturequest-' + name + '-settings-v1', dict(LEGACY_GRAPHICS_CORE_DEFAULTS))
    for name in _GRAPHICS_PROFILE_NAMES
}
V2_GRAPHICS_DEFAULT_PROFILES = {
    name: ('texturequest-' + name + '-settings-v2', dict(GRAPHICS_CORE_DEFAULTS))
    for name in _GRAPHICS_PROFILE_NAMES
}
for _profiles, _world in ((LEGACY_GRAPHICS_DEFAULT_PROFILES, LEGACY_GRAPHICS_WORLD_DEFAULTS),
                          (V2_GRAPHICS_DEFAULT_PROFILES, GRAPHICS_WORLD_DEFAULTS)):
    for _profile in _profiles:
        _schema, _values = _profiles[_profile]
        if '-world' in _profile: _values.update(_world)
        if '-aio' in _profile: _values.update(GRAPHICS_AIO_DEFAULTS)
# Freeze the exact v3 lease authority; changing presets must not change the
# values an older installation is allowed to restore.
V3_GRAPHICS_WORLD_DEFAULTS = {('EQ2Graphics', key): value for key, value in {
    'AmbientKept': '1.0',
    'AmbientOcclusion': '1',
    'AnisotropicFiltering': '1',
    'Anisotropy': '16',
    'CharacterShadows': '1',
    'EmissiveLights': '1',
    'EmissiveLightStrength': '1.0',
    'OcclusionRadius': '0.35',
    'OcclusionStrength': '1.25',
    'OwnLampShaders': '0',
    'PaintedEmissionKept': '1.0',
    'PaintedEmissionLight': '0.25',
    'PointLightShadows': '1',
    'ShadowedLights': '64',
    'PointShadowSoftness': '1.6',
    'ShadowStrength': '1.0',
    'SmoothIndoorTransition': '1',
    'SmoothInterfaceText': '0',
    'SunLighting': '0',
    'SunShadowDirectionFix': '0',
    'SunShadowMode': '0',
    'SunSoftness': '0.0',
    'TextureQuestSunSoftnessPreferenceRealSun': '1.6',
    'TextureQuestPresetVersion': '2',
    'ParticleShadowLimit': '8',
    'EmissiveLightLimit': '32',
    'ParticleLightLimit': '16',
    'EmissiveParticles': '1',
    'EmissiveObjects': '0',
    'ParticleLightStrength': '1.0',
    'ParticleLightRadius': '1.0',
    'SmoothEmissiveLighting': '0',
    'SurfaceSaturation': '0.0',
    'TextureQuestSunSoftnessUnits': '1',
}.items()}
V3_GRAPHICS_DEFAULT_PROFILES = {
    name: ('texturequest-' + name + '-settings-v3', dict(GRAPHICS_CORE_DEFAULTS))
    for name in _GRAPHICS_PROFILE_NAMES
}
from texturequest_quality import values as _quality_values, fresh_values as _quality_fresh_values, SUN_ENABLE
GRAPHICS_WORLD_DEFAULTS = _quality_fresh_values()
GRAPHICS_WORLD_DEFAULTS.update({key: value for key, value in _quality_values('Low').items() if key != SUN_ENABLE})
GRAPHICS_DEFAULT_PROFILES = {
    name: ('texturequest-' + name + '-settings-v4', dict(GRAPHICS_CORE_DEFAULTS))
    for name in _GRAPHICS_PROFILE_NAMES
}
for _profiles, _world in ((V3_GRAPHICS_DEFAULT_PROFILES, V3_GRAPHICS_WORLD_DEFAULTS),
                          (GRAPHICS_DEFAULT_PROFILES, GRAPHICS_WORLD_DEFAULTS)):
    for _profile in _profiles:
        _schema, _values = _profiles[_profile]
        if '-world' in _profile: _values.update(_world)
        if '-aio' in _profile: _values.update(GRAPHICS_AIO_DEFAULTS)
GRAPHICS_DEFAULT_SCHEMAS = {schema: values
    for profiles in (LEGACY_GRAPHICS_DEFAULT_PROFILES, V2_GRAPHICS_DEFAULT_PROFILES,
                     V3_GRAPHICS_DEFAULT_PROFILES, GRAPHICS_DEFAULT_PROFILES)
    for schema, values in profiles.values()}


def _profile_values(profile):
    if profile in GRAPHICS_DEFAULT_PROFILES: return GRAPHICS_DEFAULT_PROFILES[profile]
    if profile == 'feeder': return SCHEMA, VALUES
    if profile == 'aio': return AIO_SCHEMA, AIO_VALUES
    if profile == 'aio2': return AIO_V3_SCHEMA, AIO2_VALUES
    raise ValueError('Unknown DLSS settings backend.')


def _parse(lines, values=VALUES, *, casefold=False):
    rows, sections, section = {key: [] for key in values}, [], ''
    canonical = {(s.casefold(), k.casefold()): (s, k) for s, k in values} if casefold else {}
    section_names = {s.casefold(): s for s, _ in values} if casefold else {}
    for index, line in enumerate(lines):
        body, _ = _ending(line)
        if body.lstrip(' \t').startswith((';', '/', '#')): continue
        header = _SECTION.fullmatch(body)
        if header:
            section = header[1].strip(' \t[]')
            section = section_names.get(section.casefold(), section)
            sections.append((index, section))
        elif body.lstrip(' \t').startswith('['):
            raise ValueError('Malformed DLSS settings section; settings preserved.')
        else:
            assignment = _assignment(line)
            key = (section, assignment[0]) if assignment else None
            if casefold and key is not None: key = canonical.get((key[0].casefold(), key[1].casefold()), key)
            if key in rows: rows[key].append(index)
    return rows, sections


def _value(key, before, values=VALUES):
    if values[key] is not None: return values[key]
    entries = _split_paths(_assignment(before)[2].strip(' \t')) if before else []
    found = [index for index, entry in enumerate(entries) if entry.split('=', 1)[0].strip(' \t') == MACRO]
    if len(found) > 1: raise ValueError('Duplicate DLSS motion-provider macro; settings preserved.')
    if found: entries[found[0]] = MACRO + '=3'
    else: entries.append(MACRO + '=3')
    return ','.join(entry.replace(',', ',,') for entry in entries)


def _preserved_graphics_defaults(lines, values):
    """Keep the renderer's effective old names and explicit legacy sun-off."""
    aliases = {'PointShadowSoftness': 'ShadowSoftness',
               'EmissiveLights': 'GlowLights', 'EmissiveLightStrength': 'GlowLightStrength',
               'PaintedEmissionKept': 'PaintedGlowKept',
               'ParticleLightStrengthV2': 'ParticleLightStrength',
               'SunSoftness': 'SunShadowSize',
               'TextureQuestSunSoftnessPreferenceRealSun': 'TextureQuestSunSoftnessPreference'}
    names = {('EQ2Graphics', key): None for key in (*aliases.values(), 'SunSoftness')}
    names.update(values)
    rows, _ = _parse(lines, names, casefold=True)
    def present(key): return rows.get(('EQ2Graphics', key), [])
    def number(key):
        from math import isfinite
        indices = present(key)
        if len(indices) != 1: return None
        raw = _assignment(lines[indices[0]])[2].split(';', 1)[0].split('#', 1)[0].strip()
        try:
            value = float(raw)
            return value if isfinite(value) else None
        except ValueError: return None
    omitted = {('EQ2Graphics', key) for key, alias in aliases.items() if present(alias)}
    # A legacy particle value migrates through the native idempotent V2 helper.
    # Do not seed a new default ahead of it and silently change physical light.
    if present('ParticleLightStrength') and not present('ParticleLightStrengthV2'):
        omitted.add(('EQ2Graphics', 'ParticleLightStrengthUnits'))
    # The native sun helper derives the remembered amount from active or legacy
    # values. A default preference must not preempt that conversion on update.
    if any(present(key) for key in ('SunSoftness', 'SunShadowSize',
                                  'TextureQuestSunSoftnessPreferenceRealSun', 'TextureQuestSunSoftnessPreference')):
        omitted.update(('EQ2Graphics', key) for key in
                       ('TextureQuestSunSoftnessPreferenceRealSun', 'TextureQuestSunSoftnessUnits'))
    softness = number('SunSoftness' if present('SunSoftness') else 'SunShadowSize')
    if (not any(present(key) for key in ('SunLighting', 'SunShadowDirectionFix', 'SmoothIndoorTransition'))
            and number('PointLightShadows') == 0 and number('AmbientOcclusion') == 0
            and softness is not None and softness <= 0):
        omitted.update(('EQ2Graphics', key) for key in
                       ('SunLighting', 'SunShadowDirectionFix', 'SmoothIndoorTransition'))
    return omitted


def prepare_dlss_config(raw, *, profile='feeder'):
    """Return prepared bytes and a JSON-safe minimal before/after key lease."""
    schema, values = _profile_values(profile)
    seed_only = schema in GRAPHICS_DEFAULT_SCHEMAS
    lines, bom = _decode(raw)
    rows, _ = _parse(lines, values, casefold=seed_only)
    if any(len(indices) > 1 for indices in rows.values()):
        raise ValueError('Duplicate affected DLSS settings; settings preserved.')
    preserved = _preserved_graphics_defaults(lines, values) if seed_only else set()
    newline = next((_ending(line)[1] for line in lines if _ending(line)[1]), '\r\n')
    original_tail = _ending(lines[-1])[0] if lines else None
    original_unterminated = bool(lines and not _ending(lines[-1])[1])
    bridged, created, changes = False, [], []
    for key in values:
        if key in preserved: continue
        rows, sections = _parse(lines, values, casefold=seed_only)
        before = lines[rows[key][0]] if rows[key] else None
        # Existing component choices belong to the user. A launcher refresh
        # supplies absent defaults but never resets their in-game controls.
        if before is not None and (seed_only or profile == 'aio2' and key in AIO_CONTROL_KEYS):
            continue
        value = _value(key, before, values)
        after = (_replace_value(before, value, scalar=values[key] is not None) if before is not None
                 else key[1] + '=' + value + newline)
        if before == after: continue
        changes.append(dict(section=key[0], key=key[1], before=before, after=after))
        if before is not None:
            lines[rows[key][0]] = after
            continue
        headers = [index for index, section in sections if section == key[0]]
        position = (next((index for index, _ in sections if index > headers[-1]), len(lines))
                    if headers else len(lines))
        if position and not _ending(lines[position - 1])[1]:
            lines[position - 1] += newline
            bridged = True
        block = [after]
        if not headers:
            block.insert(0, '[' + key[0] + ']' + newline)
            created.append(key[0])
        lines[position:position] = block
    result = _encode(lines, bom)
    journal = _seal(dict(schema=schema, changes=changes, created_sections=created,
                         tail_bridge=dict(sha256=_sha(original_tail.encode()), newline=newline)
                         if bridged and original_unterminated else None,
                         before_sha256=_sha(raw), after_sha256=_sha(result)))
    _validate(journal)
    return result, journal


def _validate(journal):
    keys = {'schema', 'changes', 'created_sections', 'tail_bridge', 'before_sha256', 'after_sha256', 'record_sha256'}
    if type(journal) is not dict or set(journal) != keys or journal['schema'] not in (SCHEMA, AIO_SCHEMA, AIO_V2_SCHEMA, AIO_V3_SCHEMA, *GRAPHICS_DEFAULT_SCHEMAS):
        raise ValueError('Invalid DLSS settings journal.')
    values = GRAPHICS_DEFAULT_SCHEMAS.get(journal['schema'])
    if values is None:
        values = (VALUES if journal['schema'] == SCHEMA else
                  AIO2_VALUES if journal['schema'] == AIO_V3_SCHEMA else AIO_VALUES)
    body = {key: value for key, value in journal.items() if key != 'record_sha256'}
    if _seal(body) != journal: raise ValueError('DLSS settings journal checksum changed.')
    import re
    for key in ('before_sha256', 'after_sha256'):
        if type(journal[key]) is not str or not re.fullmatch('[a-f0-9]{64}', journal[key]):
            raise ValueError('Invalid DLSS settings file pin.')
    sections = journal['created_sections']
    if (type(sections) is not list or any(type(s) is not str for s in sections) or
            len(sections) != len(set(sections)) or any(s not in {key[0] for key in values} for s in sections)):
        raise ValueError('Invalid DLSS section ownership.')
    changes = journal['changes']
    if type(changes) is not list or len(changes) > len(values): raise ValueError('Invalid DLSS changes.')
    seen = set()
    for change in changes:
        if type(change) is not dict or set(change) != {'section', 'key', 'before', 'after'}:
            raise ValueError('Invalid DLSS changed key.')
        key = (change['section'], change['key'])
        if key not in values or key in seen: raise ValueError('Unknown DLSS changed key.')
        seen.add(key)
        for field in ('before', 'after'):
            line = change[field]
            if line is None and field == 'before': continue
            if (type(line) is not str or len(line.encode()) > MAX_INI_BYTES or '\x00' in line or
                    any(c in _ending(line)[0] for c in '\r\n') or not _assignment(line) or _assignment(line)[0] != key[1]):
                raise ValueError('Invalid DLSS changed line.')
        if journal['schema'] in GRAPHICS_DEFAULT_SCHEMAS and change['before'] is not None:
            raise ValueError('Graphics default lease cannot overwrite an existing value.')
        expected = _value(key, change['before'], values)
        after_value = _assignment(change['after'])[2].strip(' \t')
        if values[key] is not None and change['before'] is not None:
            expected = _assignment(_replace_value(change['before'], expected, scalar=True))[2].strip(' \t')
        if after_value != expected: raise ValueError('DLSS after-value is not owned.')
    bridge = journal['tail_bridge']
    if bridge is not None and (type(bridge) is not dict or set(bridge) != {'sha256', 'newline'} or
            type(bridge['sha256']) is not str or not re.fullmatch('[a-f0-9]{64}', bridge['sha256']) or
            bridge['newline'] not in ('\r\n', '\n', '\r')):
        raise ValueError('Invalid DLSS newline ownership.')
    return values


def restore_dlss_config(raw, journal, *, preserve_controls=False):
    """Restore owned values; replacement alone transfers current AIO controls.

    The caller's normal settings transaction retains these exact current lines
    through retirement so the replacement sees user choices, not absent defaults.
    A true Uninstall leaves preserve_controls false and retains its old behavior.
    """
    values = _validate(journal)
    lines, bom = _decode(raw)
    seed_only = journal['schema'] in GRAPHICS_DEFAULT_SCHEMAS
    rows, _ = _parse(lines, values, casefold=seed_only)
    replacements, conflicts = {}, []
    for change in journal['changes']:
        key = (change['section'], change['key'])
        if preserve_controls and key in AIO_CONTROL_KEYS:
            continue
        before, after, indices = change['before'], change['after'], rows[key]
        title = '.'.join(key)
        if not indices:
            if before is not None: conflicts.append(title)
            continue
        if len(indices) != 1:
            conflicts.append(title); continue
        index = indices[0]
        current = _assignment(lines[index])[2].strip(' \t')
        if current != _assignment(after)[2].strip(' \t'):
            if before is None or current != _assignment(before)[2].strip(' \t'): conflicts.append(title)
            continue
        replacements[index] = ('' if before is None else before if lines[index] == after
                               else _replace_value(lines[index], _assignment(before)[2].strip(' \t')))
    lines = [replacements.get(index, line) for index, line in enumerate(lines) if replacements.get(index, line)]
    for section in reversed(journal['created_sections']):
        _, headers = _parse(lines, values, casefold=seed_only)
        matches = [index for index, name in headers if name == section]
        if len(matches) != 1: continue
        start = matches[0]
        end = next((index for index, _ in headers if index > start), len(lines))
        if not any(line.strip() for line in lines[start + 1:end]): del lines[start]
    bridge = journal['tail_bridge']
    if bridge and lines:
        body, ending = _ending(lines[-1])
        if ending == bridge['newline'] and _sha(body.encode()) == bridge['sha256']: lines[-1] = body
    return _encode(lines, bom), tuple(conflicts)
