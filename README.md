<p align="center"><img src="docs/images/texturequest-banner.png" alt="TextureQuest â€” an EverQuest II texture pack" width="100%"></p>

# TextureQuest

**More detail. The EverQuest II you remember.**

TextureQuest is a community texture pack for **EverQuest II Origins**. Its aim is to retain the game's original style, recognizable designs and art direction while bringing out the detail in armor, creatures, signs, buildings and terrain.

AI-assisted upscaling adds finer surface detail while keeping the familiar look of Norrath. The pack includes **24,694 textures**, mostly at **1K or 2K**, with selected large creatures and heavily stretched surfaces at **4K**. It covers equipment, characters, creatures and environments. The game's models, animations and lighting remain unchanged. Some fine detail is newly generated, and results vary by texture.

## Download and install

**[Download TextureQuest 1.7.2 for Windows](https://github.com/Maergoth/TextureQuest/releases/download/v1.7.2/TextureQuest-Installer-1.7.2-windows-x64.zip)** Â· [Latest release](https://github.com/Maergoth/TextureQuest/releases/latest)

1. Patch EQ2 Origins, then close the game and launcher. If you previously replaced PAKs, use the official patcher to restore stock files before choosing a hook.
2. Extract the app outside your game folder. Keep your existing **data** folder alongside **TextureQuest.exe**.
3. Open TextureQuest, select your game folder, then **Download / Resume**.
4. Choose an install mode and click **Install**. Wait for Ready, then launch EQ2 normally.

| Install mode | What it does |
| --- | --- |
| **Graphics hook â€” default** | Adds two files beside the game EXE; existing EXE and PAKs stay unchanged. **Numpad \*** switches Original/Upscaled. Use **Shortcut** to change the key. |
| **Replace PAK files** | Changes game archives. No graphics hook. Reinstall after patching. |

**Install once, then launch normally.** The graphics DLL loads when EQ2 starts, including after a reboot. No background watcher, service or startup task. TextureQuest can stay closed; keep the app and **data** folder in place.

**To remove Graphics hook:** close EQ2, open TextureQuest and click **Uninstall graphics hook**. Only its own two game-folder files are removed; downloads and private cache are kept. Use the official patcher to restore stock PAKs after Replace mode.

**Upgrading an old hook:** close EQ2 and uninstall the old Graphics hook in the app before installing this version. For an old Memory hook, use **Settings â†’ Uninstall old Memory hook** first. The modes cannot be combined.

**After a patch:** unchanged texture content still matches. Changed assets stay original until mappings are updated. If the patcher removes our DLL, reinstall the hook. Existing ReShade settings are preserved; a conflicting graphics proxy must be resolved before installation.

**Get app update** downloads, verifies, installs and restarts TextureQuest, keeping **data** and downloads. Close EQ2, uninstall the hook when prompted, then reinstall it after updating.

<details>
<summary>Pack contents and compatibility</summary>

Texture pack **1.2.4** includes **24,694 replacements in 310 archives**, including 312 recovered 2K Maj'Dul/Desert of Flames textures and earlier repairs. Includes corrected shared Fae/Arasai male and female bodies, Qeynos stone trim and its companion map. Other released textures and recovered Maj'Dul content are preserved. The larger generation batch is still unfinished.

Graphics-only switching passed an in-world Windows Origins test. Compatibility with every asset, other graphics add-ons and future clients is not established. Neither installation method has official approval; account-policy risk is unknown.

The game keeps its original textures. Upscales load on demand with 128 MiB CPU / 256 MiB GPU replacement-cache budgets, rather than loading both complete packs into VRAM. The current catalog has 24,208 unambiguous named entries; 486 ambiguous entries stay original. Counts describe mappings, not measured in-game coverage. Cache limits may leave other textures original.

Apps 1.3.4 and earlier need one manual upgrade; preserve the existing data folder. [How the graphics hook works](docs/GRAPHICS_HOOK.md).

</details>

## Examples

Close-ups of older Norrath, using the original game models and their material layers with matching cameras and lighting. Original textures are on the left; released TextureQuest textures are on the right. These are 3D previews; appearance in game will vary with lighting, settings and character customization.

Expand a category to see close-up comparisons.

<details open>
<summary><strong>Armor</strong></summary>

**Faydark plate â€” engraved metal and scrollwork**

![Stock and TextureQuest Faydark ornate plate armor](docs/images/armor-faydark-plate.png)

</details>

<details>
<summary><strong>Weapons and shields</strong></summary>

**Qeynos Guard â€” metalwork, gem and glowing runes**

![Stock and TextureQuest Qeynos Guard shield](docs/images/shield-qeynos-guard.png)

**Dragon Sharp â€” blade and decorated grip**

![Stock and TextureQuest Dragon Sharp weapon](docs/images/weapon-dragon-sharp.png)

</details>

<details>
<summary><strong>Monsters</strong></summary>

**The Leviathan â€” mouth, teeth and scales**

Original 1K artwork beside the released 4K texture on the complete model.

![Original 1K and TextureQuest 1.1 4K Leviathan, side by side](docs/images/monster-leviathan-main-4k.png)

**Hell Boar â€” fur, skin and tusks**

![Stock and TextureQuest Hell Boar](docs/images/monster-hell-boar.png)

**Flesh Golem â€” face, skin and stitching**

![Stock and TextureQuest Flesh Golem](docs/images/monster-flesh-golem.png)

</details>

<details>
<summary><strong>Player faces</strong></summary>

**Dark Elf â€” skin and facial detail**

![Original stock and released TextureQuest Dark Elf face, side by side](docs/images/face-dark-elf.png)

</details>

<details>
<summary><strong>Architecture and signage</strong></summary>

**Signage â€” the bakery sign**

![Stock and TextureQuest bakery sign](docs/images/signage-bakery.png)

**Wood and metal â€” a Freeport door**

![Stock and TextureQuest Freeport wooden door](docs/images/wooden-door.png)

</details>

<details>
<summary><strong>Terrain</strong></summary>

**Terrain â€” Darklight Woods grass**

![Stock and TextureQuest Darklight Woods terrain](docs/images/terrain-darklight.png)

</details>

[View the full-size gallery](docs/GALLERY.md), including details about how the comparisons were made.

## Limitations

- **Seams and tiling can become more visible.** The original low-resolution textures softened or concealed some joins, repeated patterns and mismatched material boundaries. More detail can expose them.
- **AI can get details wrong.** Small lettering, symbols, eyes and tightly packed texture regions are particularly difficult. Some source information simply does not exist at higher resolution.
- **The original geometry still matters.** Low-polygon silhouettes, UV stretching, separate body parts and LOD changes remain. Texture work cannot fix every model or shader issue.
- **Some appearances may still have issues.** Character variants, facial customization and layered materials can produce results that differ from the previews.
- **Higher-resolution textures use more resources.** Storage, loading behavior and texture memory requirements can increase. Performance depends on your settings and system; no minimum hardware or FPS improvement is claimed.
- **Patches can change assets.** PAK replacement may be overwritten. Graphics matching survives unchanged texture content; changed assets need updated mappings and remain original in the meantime.

Please [report a problem](https://github.com/Maergoth/TextureQuest/issues/new/choose) with the game version/server, zone or item/NPC, character race/gender/appearance where relevant, a close-up screenshot, and the TextureQuest version installed. Include whether **Verify installed textures** passes. Do not include account credentials or private logs.

## Updates

The installer checks the [latest release](https://github.com/Maergoth/TextureQuest/releases/latest) and fetches its required texture files. Installation still requires clicking **Install TextureQuest**.

After a game patch, reinstall PAK replacement if needed. Graphics hook matches texture content rather than archive names or positions. Changed textures stay original. Downloads are reused when reinstalling.

### Can this leave the game's PAKs untouched?

Yes. **Graphics hook** adds `d3d11.dll` and `TextureQuest.ini` beside EverQuest2.exe. Windows loads the proxy normally; our add-on substitutes matching graphics resources during drawing. It does not inject from another process or redirect game archive reads. Existing EXE, PAKs and ReShade settings are preserved. [Technical summary](docs/GRAPHICS_HOOK.md).

Neither method is officially approved by Daybreak. We cannot promise account safety.

### Can I choose only races, armor, weapons or an expansion?

The installer installs the complete pack. Separate content selections are not available because the game's archives mix equipment, characters and shared world textures.

## Technical details and contributing

The [technical guide](docs/TECHNICAL.md) covers the model and generation recipe, resolution selection, material and DDS compatibility findings, corrections, validation, and building the installer. It also explains what is available to reproduce from this repository.

For manual downloads and checksums, use the [release files](https://github.com/Maergoth/TextureQuest/releases/latest). The installer fetches and verifies the required base and update files automatically. GitHub's **Source code (zip)** does not contain the textures.


### Build the app

Use Windows, 64-bit Python 3.12 with Tk support and the pinned build dependencies:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r src\texture_swapper\requirements-build.txt
.\.venv\Scripts\python.exe build_installer.py --manifest release-manifest-1.3.7.json --version 1.7.1 --loader-runtime-dir sideload-runtime --graphics-runtime-dir graphics-runtime
```

The builder verifies both runtime rosters without modifying them. `sideload-runtime` supplies offline archive preparation and old Memory hook removal; the Graphics hook does not start its injector. [Graphics source and build instructions](graphics-native/BUILDING.md) include the modified ReShade provider. Use a fresh `--output-root` to preserve earlier builds. Rebuilt binaries are not promised to be byte-identical. Building does not install into the game or generate textures.

## Credits and ownership

TextureQuest is an unofficial community project and is not affiliated with or endorsed by the EverQuest II developers or publisher. The original game artwork, names and trademarks belong to their respective owners. Distribution of this pack does not grant ownership of or relicense the underlying game assets. Third-party software notices are included with the installer.
