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
| Character shadows | Moving-character shadows from supported point and window lights |
| Emissive lights | Lights nearby surfaces from supported stationary glowing objects |
| Sun-shadow softness | Softens native sun shadows; zero retains the game filter |
| Ambient occlusion | Darkens nearby occlusion in ambient world lighting |
| Anisotropic filtering | 16x filtering on eligible world samplers when enabled |
| Smooth interface text | Bilinear filtering for interface text; not scene antialiasing |

Character and emissive lighting depend on the point-light parent control. Emissive
lighting is an approximation, not a full multi-bounce lighting engine.

The private window-light extension preserves the captured native projection and
adds character occlusion to that light contribution. Unsupported projected-light
shaders remain native. It does not replace EQ2's directional shadow system.

The main panel has feature checkboxes and shadowed lights **Min (10), Med (50),
Max (all supported; currently 256)**. Detailed controls and diagnostics are under
**Advanced**. Defaults are shadow strength **1**, softness **0.15**, AO radius
**0.35** and strength **1.25**. The first update backs up prior numeric values;
later Advanced edits are kept. The former 0.05 softness default moves to 0.15;
other custom values are retained. Feature checkbox choices are preserved.

**Rebuild lamp lighting (experimental)** changes the game's lamp falloff and can
darken rooms. It is off by default; shadows still work with native lamp lighting.
Emissive lighting is experimental. Character and particle glows are not supported
yet. With no eligible sources detected, the toggle adds no lighting.
Its status distinguishes
no detected emitters, no active draws and observed light draws; checking it does
not prove that the current scene has eligible emitters.

**Numpad \*** (or your shortcut) switches textures and all selected graphics
off/on. Original preserves each checkbox and strength. Changes apply at a frame
boundary; work already submitted to the GPU finishes normally. Settings are kept
in ReShade.ini under `[EQ2Graphics]` and survive reinstall.

Keep native lighting, material shaders and GPU shadows enabled. The renderer
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
Capture queues at most one frame ahead while active consumers finish. GPU
completion checks, newest-frame selection and resource ownership remain enforced.
A game cap near 60 FPS can help pacing; your saved cap is preserved. Synthetic
accepted Presents do not establish EQ2 performance, monitor scanout or smoothness.

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
