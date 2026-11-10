# Owned ReShade provider build

Base: ReShade v6.8.0, commit18deaa52de0c425a78b329e9cb3c497281cd00ec,
with its pinned submodules. This is an owned source build (6.8.0.7), not the
byte-identical official binary. ReShade notices are bundled with the runtime.

The complete current overlay preserves nonzero game keyboard-hook returns
for handled Alt or F10 input while ReShade is not capturing. Ordinary keys,
including Home, follow upstream handling so the menu remains accessible.
keyboard-compatibility.patch is the historical Alt-only delta; use the complete
overlay for the final input source. Mouse/capture behavior stays unchanged.
The build retains TQ_PRIVATE_INPUT_COMPAT_TRACE=1: a shared16-event cap
logs only registration/Alt-class handled-result metadata, without key codes,
text, handles, addresses or thread IDs. This is a local diagnostic, not telemetry.
An EQ2 test must establish whether it fixes the reported menu freeze/chime.

offline-glad.patch changes only the copied build generator: reproducible mode
must honor the three exact local gl/wgl/vk XML inputs selected by the project,
instead of silently loading older packaged XML. Helper headers stay packaged;
there is no network fallback. It changes no shipped runtime behavior.

Build ReShade.sln targetReShade, Release/64-bit, VS18/v145/SDK10.0.26100.0,
GladReproducible=true, with offline private Python requirements. Seed the fresh
res/version.h from the pinned baseline; upstream increments it normally. The
sealed final input tree contains6.8.0.7. Use one MSBuild and
compiler worker. No Setup/Injector/signed target is built. The app still uses
the graphics proxy path; no watcher, remote injection or game PAK replacement.

The optional overlay-demand API lets an add-on say its registered
`reshade_overlay` callback currently draws nothing. The provider can then use
its normal GUI early return when no other overlay or menu needs drawing.
Registrations from every add-on demand drawing by default. A separate locked
registry and atomic active count handle demand changes without editing the
event vectors. Existing input, Home menu, splash, screenshot messages, preview,
statistics and callback dispatch behavior are preserved. See
[OVERLAY-DEMAND-API.md](OVERLAY-DEMAND-API.md) for the interface and limits.
