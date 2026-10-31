"""One stable shared upscale per identical original, with later corrections preferred."""
from collections import Counter, defaultdict
from texture_swapper import core


def group_key(row):
    return core._digest({'layout': row['source']['layout'], 'fingerprint': row['source']['fingerprint']})


def outcome(row):
    return tuple(row['target']['layout']), row['target']['fingerprint']


def propose(index, protected=()):
    """Choose offline; a correction's revision outranks the ordinary vote.

    protected may be IDs, or ID -> increasing correction revision. No runtime
    guessing, image regeneration, or separate human review step is required.
    """
    priorities = dict(protected) if isinstance(protected, dict) else {key: 1 for key in protected}
    for row in index['named_textures']:
        revision = row.get('correction_revision', 0)
        if type(revision) is not int or revision < 0:
            raise ValueError('Invalid correction revision')
        if revision:
            priorities[row['id']] = max(priorities.get(row['id'], 0), revision)
    protected = set(priorities)
    groups = defaultdict(list)
    for row in index['named_textures']:
        groups[group_key(row)].append(row)
    choices = []
    for key, rows in sorted(groups.items()):
        if len({outcome(r) for r in rows}) < 2:
            continue
        edits = [r for r in rows if r['id'] in protected]
        if edits:
            newest = max(priorities[r['id']] for r in edits)
            edits = [r for r in edits if priorities[r['id']] == newest]
        votes = Counter(outcome(r) for r in rows)
        preferred = min((outcome(r) for r in edits), key=lambda k: (-votes[k], k)) if edits else min(votes, key=lambda k: (-votes[k], k))
        chosen = min((r for r in rows if outcome(r) == preferred), key=lambda r: r['object_sha256'])
        choices.append(dict(group=key, members=sorted(r['id'] for r in rows),
                            outcomes=sorted({r['object_sha256'] for r in rows}),
                            selected=chosen['object_sha256'],
                            reason='intentional-edit' if edits else 'most-used-outcome-stable-tie-break'))
    return dict(schema='texturequest-shared-texture-choices-v1', texture_version=index['texture_version'],
                protected_ids=sorted(protected), correction_priority=priorities, choices=choices)


def resolve(index, choices):
    """Return one selected named row per source group and a complete alias report.

    New groups are chosen automatically with the same stable rule. A known
    later correction is never outvoted by ordinary generated copies.
    """
    if choices.get('schema') != 'texturequest-shared-texture-choices-v1':
        raise ValueError('Unsupported shared texture choices')
    decisions = {r['group']: r for r in choices['choices']}
    if len(decisions) != len(choices['choices']):
        raise ValueError('Duplicate shared texture decision')
    protected = set(choices.get('protected_ids', []))
    priorities = dict(choices.get('correction_priority', {key: 1 for key in protected}))
    computed = propose(index, priorities)
    priorities = computed['correction_priority']
    protected = set(priorities)
    automatic = {r['group']: r for r in computed['choices']}
    groups = defaultdict(list)
    for row in index['named_textures']:
        groups[group_key(row)].append(row)
    selected, aliases, pending = [], [], []
    for key, rows in sorted(groups.items()):
        if len({outcome(r) for r in rows}) == 1:
            chosen = min(rows, key=lambda r: r['object_sha256'])
        else:
            decision = decisions.get(key)
            expected = sorted(r['id'] for r in rows)
            objects = sorted({r['object_sha256'] for r in rows})
            if not decision or decision['members'] != expected or decision['outcomes'] != objects:
                decision = automatic[key]
            candidates = [r for r in rows if r['object_sha256'] == decision['selected']]
            if not candidates:
                raise ValueError('Shared choice names an unrelated DDS')
            chosen = candidates[0]
            edits = [r for r in rows if r['id'] in protected]
            newest = max((priorities[r['id']] for r in edits), default=0)
            if edits and not any(outcome(r) == outcome(chosen) for r in edits if priorities[r['id']] == newest):
                chosen = next(r for r in rows if r['object_sha256'] == automatic[key]['selected'])
        selected.append(chosen)
        aliases.extend(dict(id=r['id'], selected=chosen['object_sha256'], original=r['object_sha256']) for r in rows)
    return selected, dict(aliases=aliases, pending=pending)
