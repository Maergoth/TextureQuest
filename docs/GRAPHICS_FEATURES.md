# Graphics preview

The 1.10 preview adds component choices. It is not the public 1.9.2 release.

| Component | This preview |
| --- | --- |
| Textures | Required; existing generated-channel pack and graphics hook |
| Graphical Effects | Optional glow/bloom, off by default |
| DLSS | Unavailable; integration is still in development |

Choose components, close EQ2 and click **Apply changes**. Choices persist for
each game folder. Existing texture downloads and local DDS files are reused.

**Numpad \***, or your custom shortcut, switches all selected TextureQuest
features together. Original bypasses our textures and glow. Other effects in
your own ReShade preset retain their state. Home opens ReShade; its other
configurable shortcuts remain disabled.

Glow adds a subtle halo around bright areas. It does not illuminate nearby
surfaces or replace AO, shadows or material shaders. HDR output currently
bypasses the glow. EQ2's native graphics settings remain unchanged.

Selecting glow enables ReShade FX loading. Any enabled effects in your existing
preset can also load. This can add cost. The installer preserves your preset and
shader paths; uninstall restores only settings that still match its own changes.
Externally edited settings and unknown files are retained. Shader and texture
caches remain available for reinstalling.

Private validation: real stock ReShade compilation and a small hardware pixel
test passed, including exact off-state output and unchanged alpha. EQ2 appearance,
HUD interaction and frame-time cost still need the in-game test. No working DLSS,
frame generation, modern-shadow or AO claim is made by this preview.
