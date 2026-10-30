<p align="center"><img src="docs/images/texturequest-banner.png" alt="TextureQuest — an EverQuest II texture pack" width="100%"></p>

# TextureQuest

**More detail. The EverQuest II you remember.**

TextureQuest is a community texture pack for **EverQuest II Origins**. Its aim is to retain the game's original style, recognizable designs and art direction while bringing out the detail in armor, creatures, signs, buildings and terrain.

AI-assisted upscaling adds finer surface detail while keeping the familiar look of Norrath. The pack includes **24,693 textures**, mostly at **1K or 2K**, with selected large creatures and heavily stretched surfaces at **4K**. It covers equipment, characters, creatures and environments. The game's models, animations and lighting remain unchanged. Some fine detail is newly generated, and results vary by texture.

## Download and install

**[Download TextureQuest 1.6.0 for Windows](https://github.com/Maergoth/TextureQuest/releases/download/v1.6.0/TextureQuest-Installer-1.6.0-windows-x64.zip)** · [Latest release](https://github.com/Maergoth/TextureQuest/releases/latest)

1. Patch EQ2 Origins, then close the game and launcher. If you previously replaced PAKs, use the official patcher to restore stock files before choosing a hook.
2. Extract the app outside your game folder. Keep your existing **data** folder alongside **TextureQuest.exe**.
3. Open TextureQuest, select your game folder, then **Download / Resume**.
4. Choose an install mode and click **Install**. Wait for Ready, then launch EQ2 normally.

| Install mode | What it does |
| --- | --- |
| **Graphics hook — default** | Leaves game files unchanged. Switch Original/Upscaled with **Numpad \***. Change the key with **Shortcut**. Comparisons load on demand. |
| **Replace PAK files** | Changes game archives. No background process or runtime injection. Reinstall after patching. |

**Install once, then play normally.** Graphics hook starts automatically at Windows sign-in, including after a reboot. Launch through your usual shortcut, launcher or EXE; TextureQuest can stay closed. Keep the app and **data** folder in place.

**To disable Graphics hook:** open TextureQuest and click **Uninstall**. An open game keeps the hook until it closes. If you used Replace PAK files, restore stock files with the official patcher.


**Previously installed Memory hook?** Use **Settings → Uninstall old Memory hook**, close EQ2, then install Graphics hook. Existing Dual-load installs need a reinstall to use on-demand comparisons.

**After a patch:** if game archives changed, close EQ2, then **Uninstall → Install** to refresh the hook cache. Automatic refresh is unfinished.

**Get app update** downloads, verifies, installs and restarts TextureQuest, preserving **data** and downloads. Close EQ2 first; the app prompts you to uninstall the hook, then reinstall it after updating.

<details>
<summary>Pack contents and compatibility</summary>

Texture pack **1.2.3** includes **24,693 replacements in 310 archives**, including 312 recovered 2K Maj'Dul/Desert of Flames textures and earlier repairs. Archive bytes and URLs are unchanged. Further generation is unfinished and is not included.

Full-pack graphics switching passed an in-world Windows Origins test. Runtime injection is not officially approved; compatibility and account-policy risk are unquantified. Other clients and future patches are not fully validated.

Original comparisons use disk storage with 128 MiB CPU / 256 MiB GPU cache budgets. 42 ambiguous entries retain the correct upscale; cache limits can also leave some textures upscaled. Both complete packs are not held in VRAM.

Older apps may need manual hook removal before updating. Apps 1.3.4 and earlier need one manual upgrade; keep the existing data folder.

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
- **Game patches need a refresh.** PAK replacement may be overwritten by patching. In background mode, changed game archives or indexes require uninstalling and reinstalling the background loader to refresh its private cache; automatic refresh is unfinished.

Please [report a problem](https://github.com/Maergoth/TextureQuest/issues/new/choose) with the game version/server, zone or item/NPC, character race/gender/appearance where relevant, a close-up screenshot, and the TextureQuest version installed. Include whether **Verify installed textures** passes. Do not include account credentials or private logs.

## Updates

The installer checks the [latest release](https://github.com/Maergoth/TextureQuest/releases/latest) and fetches its required texture files. Installation still requires clicking **Install TextureQuest**.

After a game patch, close every EQ2 client and the launcher. Reinstall PAK replacement if needed. For background mode, uninstall and reinstall the background loader when the patch changes game archives or indexes. Verified texture downloads are reused.

### Can this leave the game's PAKs untouched?

Yes. App 1.4.2 offers **Background texture loader** as an optional alternative to **Replace PAK files**. It prepares private archives and redirects selected file reads inside the game process, leaving the game's PAKs, settings and shortcuts unchanged. A per-user startup watcher detects ordinary future starts of the selected EverQuest2.exe. The first in-world test succeeded on one Windows EverQuest II Origins client; other client versions and every launch path have not been validated. Uninstalling stops future attachments, and already attached clients retain the loader until they close.

We have no confirmation from Daybreak approving either approach. The 1.1 installer displays this warning:

> We don't know what's more legal, replacing the game files or hooking the process. If Daybreak hates you, they might ban you. Be warned.

### Can I choose only races, armor, weapons or an expansion?

The installer installs the complete pack. Separate content selections are not available because the game's archives mix equipment, characters and shared world textures.

## Technical details and contributing

The [technical guide](docs/TECHNICAL.md) covers the model and generation recipe, resolution selection, material and DDS compatibility findings, corrections, validation, and building the installer. It also explains what is available to reproduce from this repository.

For manual downloads and checksums, use the [release files](https://github.com/Maergoth/TextureQuest/releases/latest). The installer fetches and verifies the required base and update files automatically. GitHub's **Source code (zip)** does not contain the textures.


### Build app 1.4.2 with the bundled loader

Use Windows, 64-bit Python 3.12 with Tk support, and the pinned build dependencies. From this repository's root, use the explicit versioned manifest and runtime payload:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r src\texture_swapper\requirements-build.txt
.\.venv\Scripts\python.exe build_installer.py --manifest release-manifest-1.3.7.json --version 1.4.2 --loader-runtime-dir sideload-runtime
```

The runtime descriptor, helper, catalog, native binaries and Detours license are included under `sideload-runtime/`. The builder verifies that payload and stages it without modifying it. Omitting `--loader-runtime-dir` produces an app without an available background loader. Build outputs go under `build/` and `dist/`; use a fresh `--output-root` to keep an earlier build intact. Rebuilt executables are not promised to be byte-identical. Native source and separate rebuild instructions are in [sideload-native](sideload-native/README.md). These commands build the app; they do not generate textures or install anything into the game.

## Credits and ownership

TextureQuest is an unofficial community project and is not affiliated with or endorsed by the EverQuest II developers or publisher. The original game artwork, names and trademarks belong to their respective owners. Distribution of this pack does not grant ownership of or relicense the underlying game assets. Third-party software notices are included with the installer.
