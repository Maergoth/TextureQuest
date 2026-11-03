# TextureQuest 1.13.10 Develop handoff

This is a private Develop candidate. Public `main` and the latest public release
remain unchanged. Texture data stays at 1.3.0; the provider, AIO/DLSS and other
native payloads are retained. The generator remains paused.

Particle lights now have a separate character-shadow pool instead of depending
on the nearest 16 native moving-shadow slots. **Advanced â†’ Particle shadow
limit** selects 1â€“128 eligible particle lights and requires **Character shadows**.
For example, a selected particle in native slot 33 can receive a character map.
The existing shadow strength and softness controls now also affect emissive
shadows, including the retained legacy softness convention. Other illumination
and the existing unshadowed bounce can still make a shadowed area bright.

The pool allocates 32 cubes per bank at 256 pixels per face, up to four banks:
48 MiB per allocated bank and 192 MiB maximum depth storage, plus small buffers.
The native 16 Ã— 512-pixel moving pool is unchanged. Off, invalidated and unused
maps are retired. This covers eligible moving geometry the game submits and the
renderer retains; unsupported or unsubmitted geometry remains outside coverage.
Actual game performance is unmeasured; higher limits add memory and GPU work.

Already observed particle cells remain available briefly when native drawing
stops: two seconds at full weight, then a one-second fade, with a 256-entry cache.
Held entries do not refresh their observation time or add stale energy to fresh
energy. The cache clears on scene/epoch invalidation and is not saved across
zones or app sessions. It cannot invent particles the game has never submitted.

The 074253 capture linked the floor disk's alternating lit triangles and a wall
panel's rectangular split to covered, wrong-axis base-pass normals. The disk's
base draws had no confirmed material-lamp pass to replace them. Base passes now
leave normal-atlas coverage clear, so those pixels use the existing geometric
depth fallback. Confirmed bumped material/TBN normals remain eligible. Base-only
surfaces may lose smooth vertex-normal detail. The separate half-head, neck and
arm seams still need a live check; their cure is not established by this capture.

Evidence is in `comparison/particle-character-shadows-025-001` and
`comparison/character-normal-audit-026-001`. The 025 independent static review
passed after the off/reset, bounds and legacy-softness fixes. Capture/draw and
shader-bytecode analysis supports the base-normal admission change. The final combined strict native build, focused actual hardware-D3D11 shadow/slider
case,84 cache checks, normal portable package verification and one D-only frozen
UI smoke passed. Local installation is pending launcher closure. No GitHub push
was made; the user requested local work only. No full-game coverage, beam coverage, FPS or Sun movement-flicker
fix is claimed.
