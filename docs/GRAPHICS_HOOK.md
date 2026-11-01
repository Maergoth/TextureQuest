# Graphics hook

TextureQuest is an add-on for unmodified ReShade 6.8.0 with full add-on support.
There is no external injector, watcher, service or startup task. ReShade loads
it whenever EQ2 starts normally, including after reboot.

Downloads contain only our generated/corrected channels and lookup metadata.
The installer builds individual verified DDS files outside the game folder,
using original alpha, headers and tails from the player's own installed game.
The map builds automatically; no model-by-model mapping is needed. Reconstruction
runs during setup, not every frame. Verified cached work is reused.
It adds TextureQuest.addon64 and TextureQuest.ini beside EverQuest2.exe or in
ReShade's AddonPath. If ReShade is absent, it also adds official ReShade as
dxgi.dll and a minimal configuration. Existing full-add-on ReShade is reused;
standard ReShade and unknown proxies are preserved and installation stops.

## Texture flow

1. EQ2 reads its unchanged stock archives.
2. At GPU texture creation, the add-on hashes original mips with XXH3-128.
3. A match selects a verified loose DDS and supplies its mip chain instead.
4. Numpad * switches Original/Upscaled; Shortcut changes the key.

Upscaled is the default and needs no per-draw substitution. Original mode loads
stock copies on a worker and substitutes views within the existing 512 MiB
budget. Switching may take time while those copies load.

Identical originals share one upscale. Known later corrections take priority;
otherwise the most-used outcome wins with a stable tie-break. See the
[DDS integration contract](LOOSE_DDS.md) for persistence and patch handling.

## Updates and removal

Close EQ2 before installing, uninstalling or activating texture updates. Keep
the app and its data folder in place; the app need not remain open while playing.
Saved DDS are reused. Get app update preserves portable data.

Verify / Update textures rebuilds matching against current stock. Unchanged
content survives patches; changed sources remain original until supported.
Reinstall the hook if the patcher removes its files.

Uninstall removes only owned, unchanged files. ReShade is removed only if the
app installed it and it is unchanged. Saved DDS stay. No game PAK is modified.
Restore stock with the official patcher once if an old version replaced PAKs.

Graphics hooks are not officially approved; account-policy risk is unknown.
