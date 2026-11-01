"""Prepare replacements before the normal, guarded hook handoff."""
from texture_swapper import core
from texturequest_processes import ensure_closed
from texturequest_release import check_pause


def _saved_runtime_refresh(game, source, choices, store, state, in_use, pause):
    """Reuse a current channel preparation, never claim a fresh game census."""
    from texturequest_channels import validate_source, validate_index
    from texturequest_dds_source import pin_file
    from texturequest_texture_map import HEADER
    source = validate_source(source)
    game = core._plain(core.validate_game(game).parent)  # Directory/EXE admission only, no PAK census.
    with core._lock(state / 'operation.lock'), core._lock(state / 'sideload/helper-owner.lock'):
        ensure_closed(); check_pause(pause)
        if in_use():
            raise ValueError('Close EQ2 before refreshing its graphics runtime. The hook was kept.')
        saved = store.summary()  # Selection only; full verification still follows.
        if not isinstance(saved, dict): return None
        try:
            expected = dict(schema='texturequest-loose-dds-prepared-v1', distribution='generated-channels-only',
                texture_version=source['texture_version'], source_sha256=core._digest(source),
                index_sha256=source['index']['sha256'], choices_sha256=core._digest(choices),
                named_textures=source['named_texture_count'])
            if any(saved.get(key) != value for key, value in expected.items()): return None
            if core._plain(saved['game']) != core._plain(game): return None
            coverage = saved['coverage']
            if (any(type(coverage.get(key)) is not int for key in ('mapped_groups', 'selected_groups', 'pending_groups'))
                    or coverage['selected_groups'] != source['selected_groups']
                    or not 0 < coverage['mapped_groups'] <= coverage['selected_groups']
                    or coverage['pending_groups'] != coverage['selected_groups'] - coverage['mapped_groups']): return None
            index_path = core._plain(store.base / 'metadata' / (source['index']['sha256'] + '.json'), store.base)
            pin_file(index_path, source['index'])
            raw = index_path.read_bytes()
            import hashlib
            if len(raw) != source['index']['bytes'] or hashlib.sha256(raw).hexdigest() != source['index']['sha256']: return None
            index = validate_index(core._json(raw), source, choices)
            targets = {}
            for recipe in index['recipes']:
                check_pause(pause)
                digest = recipe['object_sha256']
                target = dict(sha256=digest, entry='dds/' + digest + '.dds',
                              bytes=128 + recipe['target']['mip_bytes'] + recipe['target']['tail_bytes'])
                if digest in targets and targets[digest] != target: return None
                targets[digest] = target
            rows = store.object_rows(saved)
            if not rows or len({row['sha256'] for row in rows}) != len(rows) or any(row != targets.get(row['sha256']) for row in rows): return None
            map_path = core._plain(saved['map']['path'], store.base)
            with map_path.open('rb') as stream:
                prefix = stream.read(HEADER.size)
            if len(prefix) != HEADER.size: return None
            header = HEADER.unpack(prefix)
            if header[0] != b'TQMAP002' or header[1:4] != tuple(map(int, source['texture_version'].split('.'))) or header[4].hex() != expected['source_sha256']: return None
            if core._load(state / 'dds-prepared.json') != saved: return None
            return saved
        except (OSError, ValueError, KeyError, TypeError):
            # Normal preparation retains its stricter source/recovery behavior.
            # No damaged object is silently adopted or overwritten by this path.
            return None


def prepare_graphics(game, source_fetch, choices, store, graphics, upgrade, *,
                     minimum, binding, in_use, auto_install=False, upgrading=False, components=None,
                     report=lambda _: None, overall=lambda *_: None, pause=None):
    ensure_closed()
    check_pause(pause)
    if graphics.status(game).get('installed_newer_app'):
        raise ValueError('This hook belongs to a newer TextureQuest app. It was left unchanged.')
    changed_components = False
    if components is not None:
        from texturequest_components import normalize
        components = normalize(components)
        current = graphics.status(game)
        changed_components = bool(current.get('installed') and normalize(current.get('components')) != components)
    migrate = upgrading or upgrade.needed(game) or changed_components
    snapshot = upgrade.snapshot(game) if migrate else None
    report('Checking the texture release. Your installed hook stays in place.')
    overall('Checking the texture release', 0, 0, 0, 0)
    source = source_fetch(pause=pause, minimum=minimum)
    prepared = _saved_runtime_refresh(game, source, choices, store, upgrade.state, in_use, pause) if upgrading else None
    if prepared is None:
        prepared = store.prepare(source, game, choices, pause=pause, progress=report,
                                 overall=overall, in_use=in_use)
    else:
        report('Reusing saved texture matches for this app refresh; game coverage has not been rescanned. Use Verify / Update textures for a fresh match.')
    check_pause(pause)
    report('Verifying replacement textures before changing the installed hook…')
    overall('Verifying replacement textures', 0, 0, 0, 0)
    with core._lock(upgrade.state / 'operation.lock'), core._lock(upgrade.state / 'sideload/helper-owner.lock'):
        ensure_closed()
        if in_use():
            raise ValueError('Close EQ2 and the old loader before preparing texture updates. The hook was kept.')
        store.verify(prepared, full=True)
        if prepared.get('distribution') != 'generated-channels-only' or core._load(upgrade.state / 'dds-prepared.json') != prepared:
            raise ValueError('Replacement preparation changed. The installed hook was kept.')
        check_pause(pause)
    # No pause checkpoint between retirement and the new installation: finish
    # this short normal handoff, then honor cancellation during optional cleanup.
    if migrate:
        upgrade.before(game, report, snapshot=snapshot, pause=pause)
    else:
        if graphics.status(game).get('installed_newer_app'):
            raise ValueError('This hook belongs to a newer TextureQuest app. It was left unchanged.')
        check_pause(pause)
    if migrate or (auto_install and not graphics.status(game).get('installed')):
        # install performs its own activation under its normal ownership locks.
        # Always enter that admission after retirement: a concurrent installation
        # must refuse there, not be silently adopted or have its map redirected.
        graphics.install(game, prepared, progress=report, shortcut=binding,
                         **({} if components is None else dict(components=components)))
        upgrade.complete(game, prepared, report, pause=pause)
    else:
        graphics.activate_prepared(game, prepared)
    return source, prepared
