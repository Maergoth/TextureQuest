# Experimental shadow-transition candidate 1.13.18

This private update retains the current 1.13.16 lighting baseline and the 1.13.17
candidate. The existing comparison checkbox is relabelled **Experimental
improvements**; it is the same per-launch, unsaved switch and starts off. No saved
graphics preference is replaced. Texture data remains 1.3.0.

## Comparison

Press **Home**, choose **TextureQuest**, then **Advanced**. Leave **Experimental
improvements** unchecked for the current path and check it for the candidate.
Switches commit at frame boundaries and rebuild source caches. Compare the same
scene and camera movement, allowing at least three seconds AND 180 frames after
each switch, then longer if source caches have not settled.

**Record comparison performance (CSV)** remains a separate opt-in control. Detailed
logs and summaries go to `EQ2Graphics-captures/lighting-comparisons` beside the game.
Both comparison columns retain their samples. Changing other hook settings or
render dimensions resets the summaries. Selection and recording start off after
launch. GPU lighting timing excludes source observations and shadow-map rendering;
Game Present timing includes other game work and caps, and is not display/output
or generated-frame FPS. The existing [1.13.17 handoff](GRAPHICS-1.13.17-HANDOFF.md)
documents the log format and inherited source/performance limitations.

## New candidate scope

Valid **emissive-light** old/current endpoints blend over **150 ms** only while
the same light retains its **owner and shadow-pool slot**. Each endpoint
pairs the static map with the **actual selected moving-caster snapshot**, its source
position and matching map metadata. The selected moving map may be the particle
map or the native 512 map, with the existing particle-map priority retained. This
does not invent missing coverage from a different moving map. Current animated RGB
is used throughout; historic emission colour is not faded into current light.

Ranking turnover, a new owner or a reassigned shadow slot remains **current-only**;
this candidate does not smooth every light admission/removal. Missing, partial,
stale or incompatible old history falls back to valid current shadowing. Slot
reassignment, invalid history, settings changes, experiment epoch changes and
zone/device resets abort retained fades. Normal map update scheduling,
resolution, filter patterns, light limits and the shared shadow pool remain.

At most **six logical transitions** retain history. Private history cube slots are
allocated lazily: two 1024-face, four 512-face and eight 256-face slots, with an
**84 MiB maximum** for those retained depth textures. A full or unavailable pool
uses the current shadow path. This bound is additional candidate history storage,
not a claim about the game's complete VRAM use.

Detailed comparison CSV appends `transition_lights`. Summaries append
`mean_transition_lights`, and the UI adds **Mean transitioning lights**. These
report the actual transitioning lights in measured lighting passes. GPU lighting
timing includes transition shading but excludes source observation, shadow-map
rendering and history capture alongside those map updates. Game Present timing
includes broader game work; use the two measurements together when comparing cost.

The final renderer, strict compiler inputs and focused qualification evidence are
recorded in `graphics-renderer/RELEASE-SOURCE.json`. Earlier qualified source
observations, exact profile fallbacks and independent visibility remain in the
same candidate switch.

## Validation and limits

Final strict native compilation, focused transition/state lifecycle checks and the
normal package plus frozen changelog UI check are required before installation.
Their task receipts accompany the final renderer metadata. Live EQ2 appearance and
hardware timings still need comparison; no hardware performance improvement is
claimed. A retained moving-caster silhouette is frozen at its captured pose, so it
can briefly trail or double against the current silhouette during the 150 ms fade.
Temporal handoff is bounded stabilization, not motion-vector reprojection or a
replacement for source/map correctness.

Map availability and the shared shadow pool can limit transitions. Unsupported
materials and game-culled sources retain their existing observation limits. Sun
direction and unrelated AO issues are not claimed fixed. No Tensor Core runtime,
new texture dataset, generator operation, GitHub publication or game launch belongs
to this private package preparation.

Install only through a fresh normal updater attempt after all clients, patchers and
TextureQuest are closed. Preserve portable data and saved controls. Reuse unchanged
texture preparation; no DDS/map scan or regeneration is required.
