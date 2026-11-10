# TextureQuest 1.13.20 — Local particle-shadow motion candidate

This candidate addresses one cause of jumping particle shadows: the source was
already smoothed over 0.20 seconds, but its shadow map could remain at an old
position until the source moved more than 0.1 world units. A bounded allowance for
smaller map updates now lets more of that smooth movement reach the shadow.
Conservative particle caster caching reduces repeated CPU searches. The rejected
150 ms shadow fade remains removed.

This costs additional shadow-map rendering. A controlled software-rendered test
shows smaller shadow-position errors and jumps, but it is not an EQ2 or physical
GPU benchmark. Crowded scenes, source reassignment and the remaining map budget
can still cause steps. See the [candidate handoff](docs/GRAPHICS-1.13.20-HANDOFF.md)
for the measured tradeoff and limits. This is a local candidate, not a public
release announcement.

Choose **Low, Medium, High or Max** in the installer to configure lighting without
opening ReShade. The same **Quality Presets** dropdown is at the top of the
TextureQuest panel. **Press Home to show or hide settings.** The main panel contains
**Show FPS**, **Frame Gen** and **Quality Presets**; individual controls are in
**Advanced**, and captures, statistics and logging are in **Debug**.

| Preset | Hardware starting point | Shared shadow maps | Emissive lights | Particle lights | Particle character shadows |
| --- | --- | ---: | ---: | ---: | ---: |
| Low | RTX 2060 | 64 | 32 | 16 | 8 |
| Medium | RTX 3070 | 128 | 96 | 32 | 24 |
| High | RTX 4080 | 256 | 256 | 64 | 64 |
| Max | RTX 5090 | 512 | 512 | 128 | 128 |

All presets enable 16x anisotropic filtering, ambient occlusion, point-light
shadows, character shadows and particle emission. Medium and above also enable
object emission. High and Max add surface-light smoothing and sun processing.
Corrective sun-direction and rebuilt-lamp options remain off. Max uses the full
supported capacities; it does not inflate light brightness or radius. These are
thoughtful starting budgets, not benchmarked FPS promises. Resolution, busy raids,
native game settings and other ReShade effects also affect performance.

The qualified object/particle source fixes and independent light visibility from
1.13.19 remain active without a comparison switch. Particle grouping, source
caching and shadows tied to the position used to build their maps remain. Complete
source coverage and perfectly continuous shadow motion are not claimed. See the
[graphics guide](docs/GRAPHICS_FEATURES.md) for the other lighting tradeoffs.

Frame Gen and Neural Rendering are separate opt-ins and start off on a fresh
install. Applying a quality preset leaves those choices unchanged. Existing custom
lighting settings are preserved until you select a preset. Texture data remains
**1.3.0**; no texture download or DDS/map scan is needed when preparation is unchanged.

Close EQ2 before applying the normal update, and keep the portable data folder.
Read the [changelog](CHANGELOG.md) for the complete versioned history.
