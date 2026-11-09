# Graphical Effects / DLSS

Select **Graphical Effects / DLSS (experimental)**, close EQ2 and click **Apply changes**.
Launch normally. **Press Home to show or hide settings**, then choose **TextureQuest**.
Opening Home does not pause selected processing. Updates keep saved choices.

The component integrates [Sichii's EQ2 Graphics](https://github.com/Sichii/Eq2GraphicsAddonRS)
into EQ2's world passes before the HUD. Textures remain required.

| Feature | New-install default |
| --- | --- |
| Ambient occlusion | On |
| Anisotropic filtering | On, 16x |
| Point-light shadows | Off |
| Emissive lights | Off |
| Sun lighting | Off |
| Sun shadow softening | Off |
| Smooth interface text | Off |
| Frame Generation | Off |
| Neural Rendering | Off |

Upscaling and Reflex driver mode are removed. Old SR enable requests cannot
reactivate upscaling. TextureQuest no longer requests driver low-latency mode.

## Shadow and light quality

The main panel offers **Low, Med, Max**. Fresh budgets are Med while parent
features are off. Numeric controls remain under **Advanced**. Saved mixed
values display **Custom (Advanced)** until a preset is chosen.

| Quality | Shadow-map pool / particle character shadows | Emissive / particle lights |
| --- | --- | --- |
| Low | 10 / 10 | 16 / 16 |
| Med | 50 / 50 | 64 / 64 |
| Max | 512 / 128 | 512 / 128 |

Lighting shares the shadow-map pool; native lamps take priority. Lower shadow
quality can constrain higher light quality. These are capacity limits, not a
promise of complete scene coverage. Higher settings cost CPU/GPU time and memory.

**Point-light shadows** preserves native lamp lighting. **Character shadows**
includes supported point and projected window lights, with a separate particle
character-shadow pool up to 128. **Emissive lights** requires Point-light shadows;
its **Particles** and **Objects** choices can be selected separately. Its status
reports observed emitters and draws rather than assuming every scene is supported.

Previously seen particle sources remain for two seconds, then fade for one second.
The cache cannot discover sources the game never submits. Persistent inferred
groups and smoothed positions reduce abrupt changes; ambiguous groups can still
move differently from individual particles. A small bounded source core reduces
near-source self-occlusion spatially. It does not identify the emitter's owner;
unrelated geometry inside the small core can also be ignored.

Supported opaque shader pairs replay material normals. Unknown or unsupported
paths retain geometric fallback, so not every model seam or faceted surface is fixed.

**Ambient occlusion** adjusts ambient world lighting. Sun lighting and its separate
softening control start off. Sun-direction movement flicker remains unresolved.
**Rebuild lamp lighting** is an Advanced experiment that changes falloff/brightness;
it stays off by default. Keep native lighting, material shaders and GPU shadows
enabled. Native EQ2 INIs and the game's bloom setting remain unchanged.

## Optional DLSS processing

**Frame Generation** offers 2x / 3x / 4x on supported hardware. **Neural Rendering**
has model presets and Advanced pass controls. Both start off. Explicit saved
opt-ins survive updates; unchecking retains the preset. Motion is estimated.
More neural passes cost GPU time. A game cap near 60 FPS can help pacing.
These features do not reduce the work EQ2 already rendered.

**Show FPS** reports rendered Game frames and accepted compositor Output frames,
including generated frames. Output is not measured monitor scanout. It hides when
processing is off and can show zero during a genuine stall or initialization wait;
a guessed multiplied rate is never substituted.

Extra AIO hotkeys and window/input virtualization are disabled. The bundled
provider retains the later F10/menu fix; EQ2's own F10 UI toggle remains.
Complete appearance, coverage and performance depend on the scene and hardware.

## Switching off and updating

**Numpad \*** or your shortcut switches textures and selected graphics off/on
without losing choices. Original bypasses processing; already submitted GPU work
finishes normally. Model allocations may remain in memory.

Uncheck Point-light shadows, Ambient occlusion and Sun lighting to stop world
lighting/depth work. Filtering and textures are independent. Other ReShade effects
or add-ons may still require depth. Unknown add-ons retain it for compatibility.
Some hook overhead remains; this is not a promise of native FPS.

The installer seeds only absent defaults and versions its settings leases so older
installations can restore their exact owned settings. Later edits are retained.
The [changelog](../CHANGELOG.md) is also bundled in **What's new**.

Redistribution permission for Sichii's renderer was confirmed by the maintainer;
dependency licenses and upstream notices are included separately. This does not
claim a new upstream open-source license. Historical graphics handoffs document
earlier experiments and their validation limits.
