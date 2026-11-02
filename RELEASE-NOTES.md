TextureQuest 1.13.5

- Textures remain required; Graphical Effects is optional and experimental. Press Home for settings. Numpad * switches TextureQuest textures and effects together.
- Add world-pass ambient occlusion, point-light and character shadows, emissive material/particle lighting, sun filtering and 16x anisotropic filtering with simpler controls.
- Correct particle brightness measurement and draw-batch tracking; preserve native fire animation. Require current native shadow evidence for the sun-direction correction.
- Include compatible upstream window projection, native lamp fade, loading-scene reset and distance-consistent shadow filtering fixes.
- Separate experimental frame generation, upscaling, neural rendering and Reflex controls. These are hook-based features; results and performance vary by hardware and scene.
- Use selected graphics defaults only where settings are absent. Preserve existing choices, legacy off settings, native game INIs and reversible uninstall handling.
- Retain normal automatic app updates, prepared-before-replacement hook refresh, disabled ReShade tutorial/unwanted hotkeys and the Home settings key.

Texture data remains 1.3.0. Only generated texture channels are distributed;
the installed game supplies untouched data. No game PAK replacement is used.
Native bloom remains the player's choice. Keep native lighting and GPU shadows
enabled for the renderer's inputs. DLSS components start off; configure them in Home.

Thanks to Sichii for EQ2 Graphics and permission to include it. Third-party
components retain their respective licences and notices. New graphics features
are experimental; controlled GPU checks do not establish every scene's visual
quality, complete particle coverage or a performance gain.
