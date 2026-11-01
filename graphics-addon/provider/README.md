# Private ReShade provider build

Base: ReShade v6.8.0, commit18deaa52de0c425a78b329e9cb3c497281cd00ec,
with its pinned submodules. This is a private source build (6.8.0.1), not the
byte-identical official binary. ReShade notices are bundled with the runtime.

keyboard-compatibility.patch preserves the original game keyboard-hook return
when ReShade is not capturing input. Mouse/capture behavior stays unchanged.
The private build enables TQ_PRIVATE_INPUT_COMPAT_TRACE=1: a shared16-event cap
logs only registration/Alt-class handled-result metadata, without key codes,
text, handles, addresses or thread IDs. This is a local diagnostic, not telemetry.
An EQ2 test must establish whether it fixes the reported Alt chime.

offline-glad.patch changes only the copied build generator: reproducible mode
must honor the three exact local gl/wgl/vk XML inputs selected by the project,
instead of silently loading older packaged XML. Helper headers stay packaged;
there is no network fallback. It changes no shipped runtime behavior.

Build ReShade.sln targetReShade, Release/64-bit, VS18/v145/SDK10.0.26100.0,
GladReproducible=true, with offline private Python requirements. Seed the fresh
res/version.h to6.8.0.0; upstream increments it to6.8.0.1. Use one MSBuild and
compiler worker. No Setup/Injector/signed target is built. The app still uses
the graphics proxy path; no watcher, remote injection or game PAK replacement.
