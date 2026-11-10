# Renderer 1.13.18 private comparison build

Based on local 1.13.17 (9d50a074), with the current 1.13.16 path still the default.
Advanced > Experimental improvements is the existing unsaved A/B switch under its
new label. Opt-in comparison recording and its measurement scope are retained.

The candidate adds 150 ms same-owner/same-slot emissive handoffs, pairing each static map with the
actual selected moving-caster snapshot and corresponding source metadata. Missing,
partial or mismatched history uses valid current shadowing. Six logical transitions
use lazy history pools capped at 84 MiB. Slot reassignment, invalid history and
settings/epoch/zone changes abort the fade. Current animated RGB, source observation,
shadow resolution/filter patterns and the existing map-update budget are retained.
Ranking/slot turnover remains current-only. Frozen moving silhouettes can briefly
trail or double during the blend; this is not motion reprojection. No hardware
performance improvement is established. Comparison CSV/UI/summary output reports
transitioning-light counts.

RELEASE-SOURCE.json must record the final strict C++20 /O2 /W4 /WX inputs, binary
identity and focused evidence before the normal private package is built. Research
fixtures remain task evidence outside the shipped source tree. Software validation
does not establish live EQ2 appearance or physical-GPU FPS.

See [the comparison handoff](../docs/GRAPHICS-1.13.18-HANDOFF.md), including retained
1.13.17 source-profile limits. The normal updater preserves saved graphics settings,
portable data, texture data 1.3.0 and unchanged texture preparation.
