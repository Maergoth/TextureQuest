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


def _profile_values(profile):
    if profile == 'feeder': return SCHEMA, VALUES
    if profile == 'aio': return AIO_SCHEMA, AIO_VALUES
    if profile == 'aio2': return AIO_V2_SCHEMA, AIO_VALUES
    raise ValueError('Unknown DLSS settings backend.')


def _parse(lines, values=VALUES):
    rows, sections, section = {key: [] for key in values}, [], ''
    for index, line in enumerate(lines):
        body, _ = _ending(line)
        if body.lstrip(' \t').startswith((';', '/', '#')): continue
        header = _SECTION.fullmatch(body)
        if header:
            section = header[1].strip(' \t[]')
            sections.append((index, section))
        elif body.lstrip(' \t').startswith('['):
            raise ValueError('Malformed DLSS settings section; settings preserved.')
        else:
            assignment = _assignment(line)
            key = (section, assignment[0]) if assignment else None
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


def prepare_dlss_config(raw, *, profile='feeder'):
    """Return prepared bytes and a JSON-safe minimal before/after key lease."""
    schema, values = _profile_values(profile)
    lines, bom = _decode(raw)
    rows, _ = _parse(lines, values)
    if any(len(indices) > 1 for indices in rows.values()):
        raise ValueError('Duplicate affected DLSS settings; settings preserved.')
    newline = next((_ending(line)[1] for line in lines if _ending(line)[1]), '\r\n')
    original_tail = _ending(lines[-1])[0] if lines else None
    original_unterminated = bool(lines and not _ending(lines[-1])[1])
    bridged, created, changes = False, [], []
    for key in values:
        rows, sections = _parse(lines, values)
        before = lines[rows[key][0]] if rows[key] else None
        # Existing component choices belong to the user. A launcher refresh
        # supplies absent defaults but never resets their in-game controls.
        if (profile == 'aio2' and before is not None and key[0] == 'Standalone.DLSSNR' and
                key[1] in ('NeuralRendering', 'FrameGeneration', 'SourceResolutionOverride')):
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
    if type(journal) is not dict or set(journal) != keys or journal['schema'] not in (SCHEMA, AIO_SCHEMA, AIO_V2_SCHEMA):
        raise ValueError('Invalid DLSS settings journal.')
    values = VALUES if journal['schema'] == SCHEMA else AIO_VALUES
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


def restore_dlss_config(raw, journal):
    """Restore still-owned values; retain later edits and return conflict keys."""
    values = _validate(journal)
    lines, bom = _decode(raw)
    rows, _ = _parse(lines, values)
    replacements, conflicts = {}, []
    for change in journal['changes']:
        key = (change['section'], change['key'])
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
        _, headers = _parse(lines, values)
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
