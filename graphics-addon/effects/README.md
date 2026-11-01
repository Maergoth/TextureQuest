# TextureQuest graphics suite

Five independent techniques live in `TQEffects.fx`. Select them in ReShade. The TextureQuest master key temporarily disables all five; returning to Upscaled restores each requested component. The native controller owns this behavior. No shader installs a key or enables itself.

| Technique | What to look for | Boundary |
| --- | --- | --- |
| TQAmbientOcclusion | Darker creases and nearby intersections | Depth-based approximation; no offscreen geometry |
| TQContactShadows | Short directional shadows near visible blockers | Adjustable approximate light direction, not the game's light list |
| TQIndirectLighting | Nearby bright surfaces spill some color onto visible receivers | Screen-color bounce; brightness estimates emission, without material IDs |
| TQAntiAliasing | Softer staircase edges | Spatial smoothing; no temporal reconstruction, DLSS or frame generation |
| TQDepthDiagnostic | Raw/split depth or cyan depth edges | Debug display, normally off |

These supplement EQ2's native lighting, materials and world shadows. They cannot light unseen geometry or replace a complete lighting engine. Keep native base lighting. No source/game texture data or external shader code is included.

AO, contact shadows, bounce and debug use the genuine `DEPTH` semantic and `TQDepthReady` with `source="bufready_depth"`. The native gate validates binding availability/descriptors; shader math rejects missing readiness, invalid/out-of-screen samples and invalid controls. AO/contact/bounce treat clear depth as no surface. Unsupported cases return the original scene. Every final pass preserves its sampled original alpha, including debug. AA does not need depth.

Depth effects approximate a standard perspective camera. Near0.1, far1000 and vertical FOV75 are adjustable defaults, **not measured EQ2 camera facts**. Flip/reverse controls are explicit. Raw depth debug does not need that camera approximation; depth-edge debug does. Wrong depth selection or mapping can produce misleading effects even when a depth resource is available. There is no projection/world-alignment claim yet.

Quality is bounded: AO/bounce4–16 probes, contact6–16 ray steps. Their fields use half width/height and four-sample depth-aware upsampling. Spatial AA uses at most9 color reads. Effects support SDR with an approximate2.2 transfer curve; explicit HDR/out-of-range scene colors pass through. Screen effects can touch HUD pixels; there is no established HUD mask.

The three scratch surfaces nominally total24,883,200bytes at3840×2160, excluding driver/ReShade overhead. This is arithmetic, not measured VRAM use. Inactive techniques can retain their loaded surfaces. GPU time, FX compilation, real depth alignment and game behavior require ROOT's combined validation; CPU reference tests alone prove none of those.

Implementation is project-authored. It uses local point/depth geometry, bounded hemisphere probes, short screen-space ray tests, local color transfer and luma-gradient smoothing. It has no external include, motion history, copied shader dependency, depth writes, stencil writes or render-state changes beyond its own passes.
