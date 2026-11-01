"""Prepare replacements before the normal, guarded hook handoff."""
from texture_swapper import core
from texturequest_processes import ensure_closed
from texturequest_release import check_pause


def prepare_graphics(game, source_fetch, choices, store, graphics, upgrade, *,
                     minimum, binding, in_use, auto_install=False, upgrading=False,
                     report=lambda _: None, overall=lambda *_: None, pause=None):
    ensure_closed()
    check_pause(pause)
    if graphics.status(game).get('installed_newer_app'):
        raise ValueError('This hook belongs to a newer TextureQuest app. It was left unchanged.')
    migrate = upgrading or upgrade.needed(game)
    snapshot = upgrade.snapshot(game) if migrate else None
    report('Checking the texture release. Your installed hook stays in place.')
    overall('Checking the texture release', 0, 0, 0, 0)
    source = source_fetch(pause=pause, minimum=minimum)
    prepared = store.prepare(source, game, choices, pause=pause, progress=report,
                             overall=overall, in_use=in_use)
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
        graphics.install(game, prepared, progress=report, shortcut=binding)
        upgrade.complete(game, prepared, report, pause=pause)
    else:
        graphics.activate_prepared(game, prepared)
    return source, prepared
