# Local particle-shadow motion candidate 1.13.20

This candidate starts from public 1.13.19. Only the world renderer is replaced;
the other seven native images, including the core, are unchanged. No public
release, local installation, live-game
visual result or physical-GPU speedup is established by this document.

## Cause addressed

Particle groups already interpolate their source position over approximately
0.20 seconds. Static shadow maps previously followed that motion only after the
source moved more than 0.1 world units from the position at which its map was
built. Lighting and character shadows correctly used the built position, but
that coherent position could stand still and then jump. Close occluders magnify
the resulting change in shadow angle and receiver position.

The candidate retains source grouping and interpolation. It updates some maps
for smaller source movement so the existing smooth position can reach shadow
rendering. It does not blend shadows rendered from different origins. The failed
1.13.18 150 ms crossfade, retained history maps and dual-endpoint shading remain
absent.

## Bounded extra map work

Normal required map work runs first. New/stale maps, slot/range changes and the
existing greater-than-0.1-unit movement rule retain their scheduling. Additional
refinements apply only to shown particle lights with an existing map from an
earlier frame and a finite displacement of at least **0.005 world units** and at
most **0.1 world units**. The small threshold corresponds to 5 mm if treating a
world unit as a metre; it is defined in the renderer's world coordinates.

Each frame allows at most **eight additional cube updates** and **64 normalized
face-pixel units**, after the required updates. Each selected cube consumes
`ceil(faceSize² / 256²)` units, accounting for larger map resolutions; a 512-pixel
cube costs four units and a 1024-pixel cube costs sixteen. Every cube has six
faces, so the normalization compares like work rather than changing its face
count. The allowance bounds only additional refinements, not all pre-existing
mandatory rendering.

Candidates are ordered by oldest built frame, then displacement and stable key,
independently of hash-container iteration order. Already updated maps are not
refined twice that frame. A committed map continues to own the matching source
position/core used by direct lighting and moving-character shadow rendering.
Map resolution, shadow filters, light brightness/radius and source observation
polling are not increased globally.

Conservative particle caster envelopes reuse eligible static caster lists while
their spatial coverage and scene validity remain sufficient. This avoids a full
CPU caster search for every small movement. The envelope is a conservative
superset; it does not authorize ignoring blockers outside a cached coverage
region or retaining invalid geometry. It does not cache moving character poses.
More frequent map rasterization still has a GPU cost even when the CPU search
can be reused.

While any confirmed static caster has unknown or stale bounds, only the optional
refinements wait for the existing asynchronous measurement. Required updates
continue. This avoids multiplying conservative all-face draws during cache
initialization; unsupported bounds can prevent optional refinement. The last
eight saved captures had no confirmed casters with unknown bounds, but that does
not prove all scenes will have complete bounds.

Debug performance CSV now adds a separate `shadow_maps_snapshot` event once per
existing map-count window. Its GPU value is the last available smoothed map time,
and CPU values are averages from the separate last completed profiling window
(unavailable when CPU profiling is off). Counts include extra refinement cubes
and normalized area units. These snapshots do not enter the raw lighting-pass
summary and must not be treated as same-frame raw GPU samples.

## Controlled evidence and tradeoff

The software Direct3D WARP image fixture uses the production group-position
filter, refresh policy, caster shader and shadow filter. In an under-budget
120-frame trial with a close blocker, maximum shadow-edge error against an
every-frame reference fell from about **0.619 to 0.036 world units** (about 94%),
and maximum edge movement between frames fell from about **0.834 to 0.318**
(about 62%). Both hard and soft-shadow trials showed similar results.

That improvement required **86 logical map-build decisions instead of 10** over
120 frames; the reference chose 120. The image harness re-rasterizes maps for
deterministic observations, so these are scheduler decision counts, not a GPU
time measurement. Farther blockers showed smaller absolute errors. These results
support the identified threshold mechanism; they do not prove all source-jump
causes have been removed or predict hardware frame time.

Production-policy CPU checks cover the extra-work limits and scheduling. The
dense, rapidly moving simulated case still reached the old 0.1-unit error bound;
the allowance cannot service every source at once. A slower dense case improved
to about 0.012 units. Final native-build, integration and package evidence belongs
in the renderer provenance and fresh task receipts, not inferred from these
isolated policy/image checks.

The new recorded combat comparison was made on installed **1.13.18**, whose
candidate includes the rejected crossfade. In its final adjacent busy periods,
mean Game Present intervals improved from 11.50 to 9.77 ms, but measured lighting
time changed from 0.661 to 0.679 ms while submitted emissive lights fell from 9.67
to 7.61. These sequential workloads do not establish a firm speedup. Across the
file, admitted emissive counts stayed below 32, so it did not exercise the
high-count receiver-tile acceleration. Source observations and shadow-map work
are outside that recording's narrow lighting GPU timer. Do not attribute its
crossfade behavior or cost to public 1.13.19 or this candidate.

## Remaining limits and live comparison

- New particle group identity, owner/slot turnover, source disappearance and
  admission changes can still move shadows abruptly. Refining one retained
  group's map does not resolve an ambiguous association between different groups.
- Dense scenes can exhaust the refinement allowance. This is deliberately a
  bounded quality/cost tradeoff, not an every-map-every-frame policy.
- The hook still cannot discover sources the game never submits. Finite
  off-screen retention and unsupported material/particle paths remain.
- Static map detail, sampling noise, close-occluder sensitivity, native game
  shadow issues and unsupported material normals remain separate concerns.
- No Tensor Core backend, temporal denoiser or new source-color smoothing is
  added. Intentional source animation is preserved.

Compare a stationary lasting effect with a close blocker while moving the camera
slowly, then repeat in a crowded scene. Inspect shadow drift and source identity
alongside refinement counts/residual position error and the shadow-map timer.
Measure total frame intervals as well as the lighting pass. Preserve settings,
resolution and other clients/workloads across comparisons. A visually smoother
result may legitimately cost more map work.

## Presets, packaging and preservation

The shared Low/Medium/High/Max contract and installer/ReShade parity remain those
of 1.13.19. All presets retain 16x anisotropic filtering. Show FPS, Frame Gen and
Quality Presets remain at the top, individual controls under Advanced and
diagnostics under Debug. Frame Gen and Neural Rendering stay independent opt-ins;
an app update does not force a preset over saved custom settings.

Texture data remains 1.3.0. Build from the reviewed source freeze and explicitly
admitted renderer/provenance hashes; retain the other seven native images from
the verified public 1.13.19 runtime. Use a fresh normal updater attempt only after
all clients, patchers and TextureQuest are closed. Preserve portable data, saved
controls and native game INIs. Reuse unchanged texture preparation without a
DDS/map scan. No generator, scheduler, model or game-PAK actions belong here.
