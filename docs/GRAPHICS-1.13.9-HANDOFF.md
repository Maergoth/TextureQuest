# TextureQuest 1.13.9 private development candidate

Base: committed installed combined `21a8ef0cded7a293ee53bd7bfc8936830521a0b6`.
That normal1.13.8 update is consumed; preserve its receipts and never replay it.
This source is for local use and the new Develop branch, not a live main update
or public1.13.9tag/release. ROOT handles branch/MR integration and notification.

023 raises particle limit to128, strength to128×, radius control to16× and world
radius to128. Independent bounded draw admission allows later fitting draws
after an oversized/unsupported draw. Per-draw failures are isolated; scene,
epoch, event/map failures still fail closed. Excluded-only observations do not
refresh old energy beyond the retained TTL. The512-draw/32,768-triangle budget
is intentionally partial; draw order and native culling can affect coverage.

WORLD shadow caster gathering and the particle lookup path are corrected.
Moving character shadow maps still cover only the nearest16eligible lights;
raising the particle limit does not give all128character shadows. Surface policy
and the80-byte PR4 light ABI remain. All37 compiled inputs join the frozen023
source, strict native build and ROOT's one focused actual-D3D11 pass (all4marker
groups). The production CPU checks passed. No old alpha/full-suite replay or
raw fixture/CSO/capture copies enter Git.

Runtime selects renderer1043968B/SHA256e7254c08925b523c42fd02fd54707d0884e4e80593ceb5366f62d30e96e9c538.
The other seven native payloads, provideraf45413c/AIO96771d0f and texture1.3.0
remain unchanged. Saved controls stay separate. Full in-game coverage/appearance,
F10 cure and FPS remain unproved; Sun movement flicker is unresolved. Use the
normal private package/updater with fresh actual closure, preserve actual-time
settings/portable data, and leave generator controls paused. No C actions are
part of source/package preparation.
