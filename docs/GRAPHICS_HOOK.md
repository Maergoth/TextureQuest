# Graphics hook: technical summary

The Graphics hook is a **TextureQuest add-on for stock ReShade** (the official add-on build). It needs no modified ReShade, no background process and no private texture cache.

1. **Installation:** the app downloads a small texture map published with each release and adds `TextureQuest.addon64` and `TextureQuest.ini` to the game folder (or ReShade's `AddonPath`). If no ReShade is present it also adds the official ReShade add-on build as `dxgi.dll` (with a minimal `ReShade.ini` if none exists). An existing ReShade add-on build is used as is. A standard ReShade build cannot load add-ons; it is left unchanged and installation stops, explaining that the ReShade add-on build is needed. Any other `dxgi.dll`/`d3d11.dll` is left unchanged and installation stops.
2. **Matching:** when the game creates a texture, the add-on fingerprints its stock content (XXH3-128 over every mip, matched together with its size, mip count and format) and looks it up in the map. Ambiguous, changed or unsupported textures stay original.
3. **Loading:** a match is replaced at creation: the add-on reads that one texture from the TextureQuest archives already downloaded by the app, decompresses it, and the game receives the upscaled texture instead. Nothing runs per frame or per draw.
4. **Switching:** Numpad * (changeable in the app) switches to Original while EQ2 is focused. Original binds stock copies of the visible upscaled textures, read from the game's own archives, within a 512 MiB budget. Switching back releases them.

## Lifecycle

Install once and launch normally. TextureQuest need not run. Keep the app's data and downloads folders in place. Close EQ2 before installing or uninstalling. Download / Resume in the app updates the textures and the map together while EQ2 is closed; the hook does not need reinstalling. Uninstall removes only the files TextureQuest added and still recognises; ReShade is removed only if TextureQuest installed it and it is unchanged.

## Patching and coverage

A game patch that leaves a texture's content unchanged keeps it upscaled. A texture whose content changed stays original until a new map is published. Pack 1.2.3 maps 23,675 stock textures; 486 replacements whose stock content is shared by different upscales stay original.

## Account policy

ReShade's add-on build is not intended for games with anti-cheat that inspects loaded modules. Neither installation method has official approval; account-policy risk is unknown.

## Source

- [Add-on](../graphics-addon/BUILDING.md)
- [Installer](../src/texturequest_graphics_proxy.py)
- [Map tool](../tools/build_texture_map.py)
- [Runtime file hashes](../graphics-runtime/RUNTIME.json)
