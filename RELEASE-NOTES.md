# TextureQuest 1.13.21 — Smoother particle shadows and preserved preferences

Particle shadows now follow smaller movements of the smoothed source, reducing
the hold-and-jump behavior seen in 1.13.19. A bounded allowance for extra map
updates and conservative caster-list reuse keep that work controlled. The local
1.13.20 in-game test reported substantially less snapping. This costs additional
shadow rendering; it is not a universal FPS improvement. The failed 150 ms fade
remains removed.

**Particle light strength 1× now equals the former 5×.** Existing saved strength
is converted once so its actual brightness stays unchanged: old 5× becomes 1×,
old 1× becomes 0.2×. New profiles start at 1×. Zero stays off, and the maximum
25.6× retains the former physical maximum of 128×.

**Quality presets now preserve your visual preferences.** They change feature
enables and light/shadow capacities, without resetting brightness, light radius,
shadow strength/softness, colour, AO amounts or painted-emission balance. Sun
softening can be enabled or disabled by a preset, but its chosen amount survives.
Rebuilt-lamp and corrective sun-direction choices are also left unchanged; fresh
profiles keep those off. Editing these preferences no longer changes the preset
label to Custom. Performance settings that differ still show Custom.

Sun-shadow softness now supports three additional native shader variants found
in the supplied capture, retaining their filter alignment and the older variants.
Controlled comparisons match the native filter, and the 1–10 softness control
changes the result. The separate moving tree-shadow instability remains under
investigation; this compatibility fix is not claimed to resolve that symptom.

**Press Home to show or hide settings**, then choose TextureQuest. Show FPS,
Frame Gen and Quality Presets are at the top; individual settings are in Advanced,
and captures/statistics/logging are in Debug. The installer has the same presets.

| Preset | Starting point | Shared shadow maps | Emissive lights | Particle lights | Particle shadows |
| --- | --- | ---: | ---: | ---: | ---: |
| Low | RTX 2060 | 64 | 32 | 16 | 8 |
| Medium | RTX 3070 | 128 | 96 | 32 | 24 |
| High | RTX 4080 | 256 | 256 | 64 | 64 |
| Max | RTX 5090 | 512 | 512 | 128 | 128 |

These are engineering budgets, not measured hardware FPS guarantees. Every tier
enables 16x AF and ambient occlusion. Frame Gen and Neural Rendering are separate
opt-ins and start off. Texture data remains **1.3.0**: no new texture download or
unchanged DDS/map scan is required. Close EQ2 before applying the normal update;
keep the portable data folder. Native EQ2 INIs and bloom are unchanged.

See the [changelog](CHANGELOG.md), [graphics guide](docs/GRAPHICS_FEATURES.md) and
[developer handoff](docs/GRAPHICS-1.13.21-HANDOFF.md) for scope and limitations.
