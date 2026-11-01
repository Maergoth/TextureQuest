# Private graphics preview

Select **Graphical Effects**, close EQ2 and click **Apply changes**. Launch
normally, then open **Home → TextureQuest**. Textures are required; DLSS is optional.

The graphics component uses [Sichii's EQ2 Graphics renderer](https://github.com/Sichii/Eq2GraphicsAddonRS),
with a small private adapter for TextureQuest's master toggle and menu. It works
in EQ2's world rendering passes before the HUD, replacing our old screen-space
AO/contact implementation.

| Control | Function |
| --- | --- |
| Point-light shadows | Lamp lighting and shadows, with strength/range/count controls |
| Character shadows | Moving-character shadows from those lights |
| Emissive lights | Light estimated from recognized glowing meshes |
| Sun-shadow softness | Softens native sun shadows; zero retains the game filter |
| Ambient occlusion | Darkens nearby occlusion in ambient world lighting |
| Anisotropic filtering | Up to 16x filtering on eligible world samplers |
| Smooth interface text | Bilinear filtering for interface text; not scene antialiasing |

Character and emissive lighting depend on the point-light parent control. Emissive
lighting is an approximation, not a full multi-bounce lighting engine.

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
The previous EQ2 frame-generation test showed a performance regression. This
candidate removes a pacing feedback loop, pipelines optical-flow preparation
and uses independently fenced GUI slots. One matched isolated 3840×2054 test
increased accepted output from 96 to 128 FPS at 2x and from 131 to 209 FPS at 4x.
These are synthetic compositor measurements, not EQ2 or monitor scanout results.

Home does not pause selected processing. Original bypasses it, allowing submitted
work to drain. Model allocations may remain in memory. Extra AIO hotkeys and game
window/input virtualization are disabled.

**Show FPS** reports rendered Game frames and accepted compositor Output frames,
including generated frames. Output is not measured monitor scanout.

This is a private local candidate, not a public release. SDK/CPU tests do not
establish real EQ2 visuals, compatibility or performance. The renderer's upstream
sources are pinned at `4a656a9492584c10ab38f30391b7e99538475f87`; its author has
not supplied a top-level redistribution license, so it must not be included in
a public TextureQuest release without permission.
