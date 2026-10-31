TextureQuest 1.8.0 rebuilds Graphics hook as an add-on for stock ReShade.

- Adds TextureQuest.addon64 and TextureQuest.ini beside EQ2, plus the official ReShade add-on build as dxgi.dll if no ReShade is present. No modified ReShade, background process or private texture cache.
- Existing ReShade installs are never changed. An existing ReShade add-on build is used as is. A standard ReShade build is left unchanged and installation stops, explaining that the ReShade add-on build is needed. Any other dxgi.dll/d3d11.dll is left unchanged and installation stops. On uninstall, ReShade is removed only if TextureQuest installed it and it is unchanged.
- Matching textures are swapped as the game creates them, read from the downloaded archives using a small texture map published with the release. Nothing runs per frame or per draw.
- Numpad * still switches Original/Upscaled; change the key with Shortcut. Original loads stock copies of the visible textures on demand.
- The 1.2.3 map covers 23,675 stock textures; 486 replacements whose stock content is shared by different upscales stay original. Unchanged texture content survives game patches; changed textures stay original until a new map is published.

Close EQ2, uninstall the 1.7.0 hook, then install Graphics hook with this app. Old Memory hook removal remains under Settings. Replace PAK files remains an exclusive alternative.

Texture pack 1.2.3 and all released texture archive bytes/URLs are unchanged. Generator work is unaffected. ReShade's add-on build is not intended for games with anti-cheat that inspects loaded modules. Neither install mode is officially approved; account-policy risk is unknown.

[Technical summary and source](https://github.com/Maergoth/TextureQuest/blob/v1.8.0/docs/GRAPHICS_HOOK.md).
