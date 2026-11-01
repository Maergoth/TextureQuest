# Private graphics preview

Select **Graphical Effects**, close EQ2 and click **Apply changes**. Launch
normally, then open **Home → TextureQuest**. Textures are required; DLSS is optional.

The graphics component uses [Sichii's EQ2 Graphics renderer](https://github.com/Sichii/Eq2GraphicsAddonRS),
with a small private adapter for TextureQuest's master toggle and menu. It works
in EQ2's world rendering passes before the HUD, replacing our old screen-space
AO/contact implementation.

| Control | Function |
| --- | --- |
| Point-light shadows | Adds shadows while preserving native lamp lighting by default |
| Character shadows | Moving-character shadows from those lights |
| Emissive lights | Light estimated from recognized glowing meshes |
| Sun-shadow softness | Softens native sun shadows; zero retains the game filter |
| Ambient occlusion | Darkens nearby occlusion in ambient world lighting |
| Anisotropic filtering | 16x filtering on eligible world samplers when enabled |
| Smooth interface text | Bilinear filtering for interface text; not scene antialiasing |

Character and emissive lighting depend on the point-light parent control. Emissive
lighting is an approximation, not a full multi-bounce lighting engine.

The main panel has feature checkboxes and shadowed lights **Min (10), Med (50),
Max (all supported; currently 256)**. Detailed controls and diagnostics are under
**Advanced**. Defaults are shadow strength **1**, softness **0.05**, AO radius
**0.35** and strength **1.25**. The first update backs up prior numeric values;
later Advanced edits are kept. Feature checkbox choices are preserved.

**Rebuild lamp lighting (experimental)** changes the game's lamp falloff and can
darken rooms. It is off by default; shadows still work with native lamp lighting.
Emissive lighting only affects recognized glowing meshes. Its status distinguishes
no detected emitters, no active draws and observed light draws; checking it does
not prove that the current scene has eligible emitters.

**Numpad \*** (or your shortcut) switches textures and all selected graphics
off/on. Original preserves each checkbox and strength. Changes apply at a frame
boundary; work already submitted to the GPU finishes normally. Settings are kept
in ReShade.ini under `[EQ2Graphics]` and survive reinstall.

Keep native lighting, material shaders and world shadows enabled. The renderer
needs their inputs. This preview changes no native EQ2 INI settings. The existing
backup/restore manager remains available for future verified overrides.
Bloom, screen-color bounce, contact-only shaders and edge smoothing are retired.
Unrelated ReShade effects and settings are preserved.

## Experimental DLSS

**Home → TextureQuest** has independent checkboxes; unchecking keeps the preset:

- **Frame Generation:** 2x / 3x / 4x on supported hardware.
- **Upscaling:** Native/DLAA, Quality, Balanced, Performance or Ultra Performance.
- **Neural Rendering:** model presets and Advanced model/pass controls.

Lower presets reduce the captured image's processing resolution. They do not
reduce the rendering work EQ2 already performed, so a performance gain is not
guaranteed. Motion is estimated from images. More neural passes cost GPU time.
The previous EQ2 frame-generation test showed a performance regression. The
candidate now permits bounded capture-ahead in its existing three slots, retaining
GPU-completion checks and newest-frame selection. It also includes the earlier
pacing, optical-flow preparation and independent GUI-slot corrections. The old
test's roughly 64 FPS source hid this capture bottleneck; the final comparison
uses a faster measured source. Synthetic accepted Presents remain distinct from
EQ2 performance, monitor scanout and perceived smoothness.

Home does not pause selected processing. Original bypasses it, allowing submitted
work to drain. Model allocations may remain in memory. Extra AIO hotkeys and game
window/input virtualization are disabled.

**Show FPS** reports rendered Game frames and accepted compositor Output frames,
including generated frames. Output is not measured monitor scanout.
Output hides when processing is off and reads zero during a genuine output stall
or initialization wait; it is never replaced with a guessed multiplied rate.

This is a private local candidate, not a public release. SDK/CPU tests do not
establish real EQ2 visuals, compatibility or performance. The renderer's upstream
sources are pinned at `4a656a9492584c10ab38f30391b7e99538475f87`; its author has
not supplied a top-level redistribution license, so it must not be included in
a public TextureQuest release without permission.
