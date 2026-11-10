# Renderer 1.13.16 local development build

Based on public 1.13.15. Object cache measurements are placed only in observed
world cells and combined by conserved area-weighted flux. A tiny, unambiguous
adjacent-cell correction handles grid-boundary reprojection rounding. Unknown
regions receive no parent or whole-object colour fallback.

Known mesh bounds conservatively skip nonintersecting static cube-shadow faces.
Map resolution, samples, moving shadows and update scheduling are unchanged.
Invalid reconstruction samples are rejected. Optional slope-aware four-tap
surface smoothing is compiled separately and defaults off. It uses no history
and does not filter source animation or the game texture colour.

The strict C++20 /O2 /W4 /WX build passed. RELEASE-SOURCE.json records actual
compiler inputs, binary identity and focused CPU/software Direct3D evidence.
The alternate squared-range shader optimization was not adopted because
controlled software timings regressed for inside-heavy light workloads.

These checks do not establish physical-GPU performance or a complete fix in
EQ2. See [the candidate handoff](../docs/GRAPHICS-1.13.16-HANDOFF.md). Earlier
particle grouping, material normals, off-screen retention and bounded source
cores are retained; their evidence in metadata is explicitly historical.
