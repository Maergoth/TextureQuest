# Graphical Effects / DLSS

Select **Graphical Effects / DLSS**, choose a **Quality Presets** setting, close EQ2
and click **Apply changes** in the launcher. The quality dropdown also works for
an existing installed hook without rebuilding textures. Launch normally.
**Press Home to show or hide settings**, then choose **TextureQuest**. Opening Home
does not pause processing.

The main panel contains **Show FPS**, **Frame Gen** and **Quality Presets**. Individual
controls are in **Advanced**. Captures, diagnostic views, statistics and performance
logging are in **Debug**. The active controls no longer carry experimental labels;
remaining implementation limits are documented here.

The component integrates [Sichii's EQ2 Graphics](https://github.com/Sichii/Eq2GraphicsAddonRS)
into EQ2's world passes before the HUD. Textures remain required.

## Quality presets

The installer and in-game panel consume the same versioned preset definition.
**Low** is the new-install default. An update keeps saved/custom values until you
explicitly choose a preset. Manual changes that differ from a complete preset show
**Custom**; this is a status rather than a fifth preset.

| Preset | Hardware starting point | Shared shadow maps | Emissive lights | Particle lights | Particle character shadows |
| --- | --- | ---: | ---: | ---: | ---: |
| Low | RTX 2060 | 64 | 32 | 16 | 8 |
| Medium | RTX 3070 | 128 | 96 | 32 | 24 |
| High | RTX 4080 | 256 | 256 | 64 | 64 |
| Max | RTX 5090 | 512 | 512 | 128 | 128 |

| Feature | Low | Medium | High | Max |
| --- | --- | --- | --- | --- |
| Anisotropic filtering | 16x | 16x | 16x | 16x |
| Ambient occlusion | On | On | On | On |
| Point-light / character shadows | On | On | On | On |
| Particle emission | On | On | On | On |
| Object emission | Off | On | On | On |
| Surface-light smoothing | Off | Off | On | On |
| Sun lighting / shadow softening | Off | Off | On | On |
| Rebuild lamp lighting / corrective sun direction | Off | Off | Off | Off |
| Smooth interface text | Off | Off | Off | Off |

These tiers are engineering starting points, not benchmarked FPS guarantees. Pixel
resolution, crowds, the CPU, multiple clients and other ReShade effects change the
cost. Low limits moving-shadow work and avoids whole-object emission sampling.
Medium adds object lighting with a moderate moving-shadow pool. High increases
coverage and enables the optional surface reconstruction filter and sun effects.
Max opens the full supported budgets; it does not inflate artistic intensity or
radius. Higher limits can be expensive in dense scenes even on the named GPU.

All tiers keep the same light brightness/radius and shadow-strength/softness tuning.
The shared shadow-map pool includes native and inferred lights; native lamps take
priority. An emissive light is eligible only while a suitable shared slot exists.
Consequently the displayed capacities are ceilings, not guaranteed simultaneous
counts. Particle moving shadows additionally consume their own pool.
Lower presets reduce per-frame lighting and shadow work, but the current renderer
reserves roughly 1 GiB of base shadow textures regardless of tier, before particle
shadow allocations. Reducing these count limits does not proportionally reduce
VRAM use.

Frame Gen and Neural Rendering are independent. They start off on a new install;
applying any lighting preset leaves their current choices unchanged. Presets also
leave Show FPS and diagnostic recording untouched.

## Lighting behavior and limits

**Point-light shadows** preserves native lamp lighting. **Character shadows**
includes supported point and projected window lights, with a separate particle
character-shadow pool up to 128. **Emissive lights** requires Point-light shadows;
its **Particles** and **Objects** choices can be selected separately. Debug reports
observed emitters and draws rather than assuming every effect is supported.

Previously seen particle sources remain for two seconds, then fade for one second.
The cache cannot discover sources the game never submits. Persistent groups and
smoothed positions reduce source changes, but ambiguous groups can still move or
change shadow admission. The 150 ms shadow crossfade from local 1.13.18 has been
removed after visual testing; this release does not claim perfectly continuous
shadows. A small bounded source core reduces near-source self-occlusion spatially.
It does not identify the emitter's owner; unrelated geometry inside the small core
can also be ignored.

The promoted qualified source observations preserve animated material colour and
alpha, while reducing camera-dependent measurements for the supported profiles.
Unsupported profiles retain their previous path. Each admitted light now uses its
own shadow visibility. Conservative receiver tiles skip negligible contributions;
this correct visibility can cost more than the old shared-visibility shortcuts in
dense scenes. Static shadow-face culling avoids geometry that cannot affect a face.
These changes do not imply a universal speedup.

**Smooth emissive lighting** filters the reconstructed lighting contribution near
valid related surfaces; it does not blur texture colour or use frame history.
It costs extra sampling work and remains individually adjustable in Advanced.
Supported opaque shader pairs replay material normals. Unknown paths retain
geometric fallback, so not every model seam or faceted surface is fixed.

Sun lighting and shadow softening are separate from the corrective sun-direction
override. The latter has unresolved movement flicker and remains off in every
preset. **Rebuild lamp lighting** changes falloff/brightness and also stays off.
Keep native lighting, material shaders and GPU shadows enabled. Native EQ2 INIs
and the game's bloom setting remain unchanged.

## Optional DLSS processing and diagnostics

**Frame Gen** offers 2x / 3x / 4x on supported hardware. **Neural Rendering** in
Advanced has model presets and pass controls. Explicit saved opt-ins survive
updates. Motion is estimated; more neural passes cost GPU time. A game cap near
60 FPS can help pacing. These features do not reduce work EQ2 already rendered.
Upscaling and Reflex driver mode are removed; legacy requests cannot reactivate them.

**Show FPS** reports rendered Game frames and accepted compositor Output frames,
including generated frames. Output is not measured monitor scanout. Output hides
when processing is off and can show zero during a genuine stall or initialization;
a guessed multiplied rate is never substituted.

Debug performance recording is opt-in and saves CSV/summary files under
`EQ2Graphics-captures/lighting-performance` beside the game. GPU lighting timing covers light-list
construction, lighting and reconstruction, excluding source observations and map
rendering. Game Present intervals include broader work and frame caps. Compare
like scenes, resolutions, active light counts and settings, allowing warm-up.
These records alone do not establish visual quality or a hardware speedup.

Extra AIO hotkeys and window/input virtualization remain disabled. The bundled
provider retains the later F10/menu fix; EQ2's own F10 UI toggle remains.

## Switching off and updating

**Numpad \*** or your shortcut switches textures and selected graphics off/on
without losing choices. Original bypasses processing; submitted GPU work finishes
normally. Model allocations may remain in memory.

Uncheck Point-light shadows, Ambient occlusion and Sun lighting to stop world
lighting/depth work. Filtering and textures are independent. Other ReShade effects
or add-ons may still require depth. Some hook overhead remains.

The normal updater preserves portable data and unchanged texture preparation.
The [changelog](../CHANGELOG.md) is bundled in **What's new**. Developer preset
maintenance and release qualification are described in the [1.13.19 handoff](GRAPHICS-1.13.19-HANDOFF.md).

Redistribution permission for Sichii's renderer was confirmed by the maintainer;
dependency licenses and upstream notices are included separately. This does not
claim a new upstream open-source license. Historical graphics handoffs document
earlier local experiments and their validation limits.
