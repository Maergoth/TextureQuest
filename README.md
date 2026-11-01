<p align="center"><img src="docs/images/texturequest-banner.png" alt="TextureQuest — an EverQuest II texture pack" width="100%"></p>

# TextureQuest

**More detail. The EverQuest II you remember.**

TextureQuest is a community texture pack for **EverQuest II Origins**. Its aim is to retain the game's original style, recognizable designs and art direction while bringing out the detail in armor, creatures, signs, buildings and terrain.

AI-assisted upscaling adds finer surface detail while keeping the familiar look of Norrath. The pack includes **24,694 textures**, mostly at **1K or 2K**, with selected large creatures and heavily stretched surfaces at **4K**. It covers equipment, characters, creatures and environments. The game's models, animations and lighting remain unchanged. Some fine detail is newly generated, and results vary by texture.

## Download and install

**[Download TextureQuest for Windows](https://github.com/Maergoth/TextureQuest/releases/latest)**

1. Patch EQ2 Origins, then close the game and launcher. If you previously replaced PAKs, use the official patcher to restore stock files before installing the hook.
2. Extract the app outside your game folder. Keep your existing **data** folder alongside **TextureQuest.exe**.
3. Open TextureQuest, select your game folder, then click **Install graphics hook**. It downloads and prepares everything automatically.
4. Launch EQ2 normally. **Numpad \*** switches Original/Upscaled; use **Shortcut** to change it.

The installer downloads only our generated/corrected channels from [TextureQuest-Textures](https://github.com/Maergoth/TextureQuest-Textures/releases/latest), about **17.7 GB**. It reads protected channels, headers and tails from your own game files and builds a **27.8 GB local texture cache** plus the lookup map automatically. No manual mapping, extra tools or game PAK changes. Setup reuses verified cached files; reconstruction does not run every frame.

**Install once, then launch normally.** ReShade loads the add-on when EQ2 starts, including after a reboot. No background watcher, service or startup task. TextureQuest can stay closed; keep the app, **data** and downloads in place.

**To remove Graphics hook:** close EQ2, open TextureQuest and click **Uninstall graphics hook**. Only the files it added and still recognises are removed; ReShade is removed only if TextureQuest installed it and it is unchanged. Saved DDS files are kept. If you previously used Replace mode, use the official patcher once to restore stock PAKs.

**Upgrading:** close EQ2 and update the app. Setup resumes automatically, replaces old owned hooks and removes verified obsolete PAK/DDS caches after success. Custom textures, settings and receipts are kept. If a step needs attention, use **Finish upgrade**. Changed or unknown files are preserved.

**Previously used Replace PAK files?** If setup asks, run the official EQ2 launcher → **Advanced Tools → Validate Game Assets** once. Close the launcher, then click **Finish upgrade**. TextureQuest cannot restore original game files itself.

ReShade's tutorial bar and stats are hidden by default. **Home** opens/closes its panel. Other configurable ReShade shortcuts are disabled, including screenshots, preset switching and effect/uniform hotkeys. **Numpad \*** (or your custom shortcut) remains the TextureQuest switch. **Settings → Reset ReShade shortcuts** reapplies these defaults with EQ2 closed. The brief ReShade startup notice may still appear.

**After a patch:** unchanged texture content still matches. Changed assets stay original; **Verify / Update textures** rebuilds the map against your current stock files. New artwork for a changed source requires a compatible texture release. If the patcher removes our files, reinstall the hook. An existing ReShade add-on build is used as is. A standard ReShade build is left unchanged and installation stops, explaining that the ReShade add-on build is needed. Any other `dxgi.dll`/`d3d11.dll` is left unchanged and installation stops.

**Get app update** downloads, verifies, installs and restarts TextureQuest, keeping **data**. Every new app version automatically reinstalls its owned hook and reapplies ReShade defaults, even if the hook is already present. The tutorial stays disabled. Close EQ2 when prompted; saved textures are reused. Changed or unknown files are preserved.

<details>
<summary>Pack contents and compatibility</summary>

Texture pack **1.3.0** includes **24,694 named replacements**, including 312 recovered 2K Maj'Dul/Desert of Flames textures and earlier repairs. Includes corrected shared Fae/Arasai male and female bodies, Qeynos stone trim and its companion map. Other released textures and recovered Maj'Dul content are preserved. The larger generation batch is still unfinished.

Compatibility with every asset, other graphics add-ons and future clients is not established. ReShade's add-on build is not intended for games with anti-cheat that inspects loaded modules. This graphics hook has no official approval; account-policy risk is unknown.

Matching DDS files replace stock data when EQ2 creates GPU textures. The stock-ReShade add-on and its Original/Upscaled hotkey remain. Upscaled mode needs no per-draw replacement work. Original mode loads stock copies on demand within a 512 MiB budget.

Identical originals share one upscale: the most-used existing version, with a stable tie-break. Known later corrections take priority. The existing 208 conflicting groups (486 named files) now have consistent selections; all named identities and categories remain in the dataset. These are dataset counts, not measured in-game coverage.

Apps 1.3.4 and earlier need one manual upgrade; preserve the existing data folder. [How the graphics hook works](docs/GRAPHICS_HOOK.md).

</details>

## Examples

Close-ups of older Norrath, using the original game models and their material layers with matching cameras and lighting. Original textures are on the left; released TextureQuest textures are on the right. These are 3D previews; appearance in game will vary with lighting, settings and character customization.

Expand a category to see close-up comparisons.

<details open>
<summary><strong>Armor</strong></summary>

**Faydark plate — engraved metal and scrollwork**

![Stock and TextureQuest Faydark ornate plate armor](docs/images/armor-faydark-plate.png)

</details>

<details>
<summary><strong>Weapons and shields</strong></summary>

**Qeynos Guard — metalwork, gem and glowing runes**

![Stock and TextureQuest Qeynos Guard shield](docs/images/shield-qeynos-guard.png)

**Dragon Sharp — blade and decorated grip**

![Stock and TextureQuest Dragon Sharp weapon](docs/images/weapon-dragon-sharp.png)

</details>

<details>
<summary><strong>Monsters</strong></summary>

**The Leviathan — mouth, teeth and scales**

Original 1K artwork beside the released 4K texture on the complete model.

![Original 1K and TextureQuest 1.1 4K Leviathan, side by side](docs/images/monster-leviathan-main-4k.png)

**Hell Boar — fur, skin and tusks**

![Stock and TextureQuest Hell Boar](docs/images/monster-hell-boar.png)

**Flesh Golem — face, skin and stitching**

![Stock and TextureQuest Flesh Golem](docs/images/monster-flesh-golem.png)

</details>

<details>
<summary><strong>Player faces</strong></summary>

**Dark Elf — skin and facial detail**

![Original stock and released TextureQuest Dark Elf face, side by side](docs/images/face-dark-elf.png)

</details>

<details>
<summary><strong>Architecture and signage</strong></summary>

**Signage — the bakery sign**

![Stock and TextureQuest bakery sign](docs/images/signage-bakery.png)

**Wood and metal — a Freeport door**

![Stock and TextureQuest Freeport wooden door](docs/images/wooden-door.png)

</details>

<details>
<summary><strong>Terrain</strong></summary>

**Terrain — Darklight Woods grass**

![Stock and TextureQuest Darklight Woods terrain](docs/images/terrain-darklight.png)

</details>

[View the full-size gallery](docs/GALLERY.md), including details about how the comparisons were made.

## Limitations

- **Seams and tiling can become more visible.** The original low-resolution textures softened or concealed some joins, repeated patterns and mismatched material boundaries. More detail can expose them.
- **AI can get details wrong.** Small lettering, symbols, eyes and tightly packed texture regions are particularly difficult. Some source information simply does not exist at higher resolution.
- **The original geometry still matters.** Low-polygon silhouettes, UV stretching, separate body parts and LOD changes remain. Texture work cannot fix every model or shader issue.
- **Some appearances may still have issues.** Character variants, facial customization and layered materials can produce results that differ from the previews.
- **Higher-resolution textures use more resources.** Storage, loading behavior and texture memory requirements can increase. Performance depends on your settings and system; no minimum hardware or FPS improvement is claimed.
- **Patches can change assets.** Graphics matching survives unchanged texture content; changed assets need updated mappings and remain original in the meantime.

Please [report a problem](https://github.com/Maergoth/TextureQuest/issues/new/choose) with the game version/server, zone or item/NPC, character race/gender/appearance where relevant, a close-up screenshot, and the TextureQuest version installed. Include whether **Verify installed textures** passes. Do not include account credentials or private logs.

## Updates

The installer checks the [latest release](https://github.com/Maergoth/TextureQuest/releases/latest) and fetches its required texture files. Installing the graphics hook still requires clicking **Install graphics hook**.

After a game patch, unchanged textures keep matching. Verify / Update textures refreshes current stock matching; changed sources stay original. Saved DDS files are reused.

### Can this leave the game's PAKs untouched?

Yes. **Graphics hook** adds `TextureQuest.addon64` and `TextureQuest.ini` beside EverQuest2.exe (or ReShade's AddonPath), plus the official ReShade add-on build as `dxgi.dll` (with a minimal `ReShade.ini` if none exists) if no ReShade is present. ReShade loads the add-on normally; it swaps matching textures when the game creates them. It does not inject from another process or redirect game archive reads. Existing EXE, PAKs and ReShade settings are preserved. [Technical summary](docs/GRAPHICS_HOOK.md).

The graphics hook is not officially approved by Daybreak. We cannot promise account safety.

### Can I choose only races, armor, weapons or an expansion?

The installer currently uses the complete pack. Categories and map types are retained in the DDS index for future selection controls; shared textures can affect several categories.

## Technical details and contributing

The [technical guide](docs/TECHNICAL.md) covers the model and generation recipe, resolution selection, material and DDS compatibility findings, corrections, validation, and building the installer. It also explains what is available to reproduce from this repository.

For manual downloads and checksums, use the [release files](https://github.com/Maergoth/TextureQuest/releases/latest). The installer fetches and verifies the required base and update files automatically. GitHub's **Source code (zip)** does not contain the textures.


### Build the app

Use Windows, 64-bit Python 3.12 with Tk support and the pinned build dependencies:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r src\texture_swapper\requirements-build.txt
.\.venv\Scripts\python.exe build_installer.py --manifest src/release-manifest.json --version 1.9.0 --graphics-runtime-dir graphics-runtime
```

The builder pins the graphics runtime and shared-texture choices. No offline PAK preparation tools are bundled. The legacy release manifest remains small app-update metadata; texture downloads come from the independent DDS source. See [the integration contract](docs/LOOSE_DDS.md) and [add-on build instructions](graphics-addon/BUILDING.md). Build into a fresh `--output-root`. Building does not install into EQ2 or generate textures.

## Credits and ownership

TextureQuest is an unofficial community project and is not affiliated with or endorsed by the EverQuest II developers or publisher. The original game artwork, names and trademarks belong to their respective owners. Distribution of this pack does not grant ownership of or relicense the underlying game assets. Third-party software notices are included with the installer.
