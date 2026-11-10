# 1.13.21 developer handoff

This release promotes the locally approved 1.13.20 particle-shadow motion path,
separates performance presets from appearance preferences, rebases particle
strength, and restores softness filtering for three captured native sun variants.
The previous 150 ms crossfade remains removed. Texture data stays 1.3.0.

## Particle motion and strength

The existing 0.20-second source-position smoothing is unchanged. Shadow-map
refinements can now follow movements from 0.005 to 0.1 world units after required
map work, with at most eight extra cubes and 64 normalized face-area units.
Conservative caster-list reuse avoids repeated gathers while bounds remain valid.
Unknown or stale bounds stop optional refinements; required work still proceeds.
The map, light position and source core remain a coherent committed snapshot.
See the 1.13.20 handoff for the controlled motion/error tradeoff.

Particle strength is now expressed in fivefold units. `ParticleLightStrengthV2`
is authoritative even if its version marker was interrupted; otherwise the old
`ParticleLightStrength` is converted by dividing by five. The old key stays
untouched for history/rollback. Repeated loads do not divide again. A genuinely
absent value starts at new 1; explicit zero stays zero. New maximum 25.6 equals
old maximum 128 physically. Object emission strength does not change.

## Preset ownership

The canonical JSON and generated C++ define 15 performance-owned values and
separate fresh appearance defaults. Both the launcher and native menu preserve
appearance choices during tier changes. `SunShadowSoftening` is a logical enable
intent, never a stored second boolean: native `SunSoftness` remains the active
amount, with the existing remembered preference restored when re-enabled.
Legacy softness conversion uses the existing native unit rules.

Fresh defaults are not a preset. Native initialization detects existing modern
or legacy world settings before any migration writes. Installer v4 leases seed
only absent values and avoid preempting legacy strength/softness conversion.
Historical v1/v2/v3 leases retain their exact dictionaries for rollback/uninstall.

## Native GPU shadows and sun softness

GPU-shadow integration predates this release. Git history has supported sun
replacement since 2112c2c; 55bc23e fixed border taps and upstream 56b592e changed
classification/softness handling. The 1.13.20 particle fix did not remove it.

The current scene uses three additional native pixel-shader variants. Exact
captured hashes are mapped to the existing near/middle/far passes while retaining
their filter origin. They differ from the older shaders by one texel of filter
alignment, so blindly aliasing hashes would shift edges. Unknown shaders still
use the native path. The native filter and replacement are compared against
captured bytecode in software Direct3D; this is not a game or hardware benchmark.

This restores softness support in that scene, but does not establish the cause
of the broad native tree-shadow shifts in the recording. The latest normal
update preserved `eq2.ini`, `eq2_recent.ini` and `eq2_default.ini` byte-for-byte.
The current installer uses an empty native override map; ReShade.ini and
TextureQuest.ini are intentionally managed. A later game save can change native
INIs. Neither a single capture nor the installer evidence rules out all runtime
interaction. Native instability also reported without the hook remains unresolved.

## Supplied performance recording

The approximately 15.17-second recording contains one or two emitted lights.
After warm-up, the lighting pass averages 0.142933 ms, p95 0.150784 ms. This timer
excludes emitter measurement and shadow-map generation. Separate shadow-map GPU
snapshots are smoothed readings, at most approximately 0.124 ms, and CPU map
windows are approximately 0.104 ms. These are not same-frame raw pairs and must
not be added into an asserted end-to-end frame-time measurement.

The capture reports active refinements and heavy caster-envelope reuse. It is
a useful low-load check, not evidence of a combat speedup or a 5090/4080/3070/2060
performance guarantee. Existing preset budgets and fixed base shadow allocation
remain. No physical-GPU benchmark or generator action was performed for release.

## Validation

Focused checks cover one-time strength migration, native/launcher preset parity,
appearance preservation, partial profiles and historical lease authority.
Strict C++ builds use warnings-as-errors and record actual compiler inputs.
The normal installer package is verified, including its runtime and source pins,
followed by a packaged UI/changelog smoke in an isolated synthetic game folder.
The release validation JSON records the final evidence and binary hashes.
