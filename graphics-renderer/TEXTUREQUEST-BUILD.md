# Renderer 1.13.17 local comparison build

Based on local 1.13.16. The current renderer remains the default. Advanced exposes
an unsaved experiment switch and opt-in comparison logging. The candidate uses
qualified whole-triangle object material observations, particle geometry before
native screen-size clamping, and independent per-light shadow visibility with
conservative receiver-tile admission. Unsupported source profiles retain the old
path. Shadow map resolution and sample patterns are unchanged.

RELEASE-SOURCE.json records actual strict C++20 /O2 /W4 /WX compiler inputs,
binary identity and focused CPU/software Direct3D evidence. Research fixtures
and performance measurements are outside the shipped source tree. Software
timings do not establish physical-GPU FPS. The handoff records rejected research
and limits; no CUDA, Tensor Core runtime or replacement shadow sampler is added.

See [the comparison handoff](../docs/GRAPHICS-1.13.17-HANDOFF.md). Earlier texture,
material-normal, retention and source-core behavior is retained. Saved graphics
settings and texture data 1.3.0 are preserved by the normal updater.
