# Graphics preview

Choose optional **Graphical Effects** and **DLSS (Experimental)**, close EQ2 and
click **Apply changes**. Textures are required. Downloads stay cached.

Use **Home → TextureQuest** for the private AIO DLSS controls:

- **Frame Generation:** checkbox on/off; **2x / 3x / 4x** selects one, two or
  three generated frames between real frames. Hardware/runtime support is required.
- **Upscaling:** checkbox on/off; **Native/DLAA, Quality, Balanced, Performance,
  Ultra Performance**. Lower presets process a reduced captured image; they do
  not reduce EQ2's original rendering cost or guarantee higher FPS.
- **Neural Rendering:** checkbox on/off; three model presets, with **Advanced**
  model/pass controls. More passes cost GPU time.

Unchecking preserves the selected preset. Choices apply while playing; Original
temporarily stops them all. Motion is estimated from images. HUD behavior,
temporal quality and combined EQ2 performance still need testing. Extra AIO
hotkeys and game window/input virtualization are disabled.

**Home** opens ReShade. Each effect has a requested checkbox and separate
On/Off/Waiting for depth status. Strength controls are below the list:

| Effect | What to look for |
| --- | --- |
| Ambient occlusion | Darkening around visible corners and close surface contact |
| Contact shadows | Short directional shadows from visible nearby geometry |
| Indirect lighting | Approximate colour bounce from bright visible surfaces |
| Bloom | A halo around bright pixels |
| Anti-aliasing | Smoother high-contrast edges, with some possible softness |
| Depth diagnostic | Debug view for finding the correct depth orientation/range |

**Numpad \*** (or your shortcut) switches all TextureQuest features off/on.
Individual ReShade choices survive this toggle and shader reload within the
session. Other people's effects are not controlled by TextureQuest.
For the selected DLSS backend, Original stops new processing and hides generated
output. Already submitted work drains; model allocations can remain in memory.

These effects use the finished screen image and available depth. They cannot
see off-screen lights/occluders or recover material/light information that EQ2
does not expose. The light direction and camera settings are approximations,
not automatically recovered game facts. AO/contact/bounce stay off without a
valid depth binding; bloom and AA can still work. HDR currently bypasses the
scene effects. Screen-space effects can affect the HUD and cost GPU time.

Keep native base lighting, materials and world shadows. No supported native
AO/AA/bloom disable command was established, so this preview changes none of
those settings. The user's existing r_light_priority value is left alone.
The native-settings manager backs up full originals before any future reviewed
edit, stores them only in portable data, and restores on uninstall. Later edits
are preserved with conflicts reported; eq2_default.ini is never modified.

Enabling effects loads ReShade FX, including enabled effects in your own preset.
Existing preset/search paths are preserved. Uninstall restores only settings
still owned by TextureQuest. Keep data and its settings journals/backups.

One combined standalone compile/pixel test and an EQ2 test qualify this preview;
CPU formulas or compilation alone do not prove EQ2 depth, appearance or FPS.
