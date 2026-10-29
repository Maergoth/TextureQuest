<p align="center"><img src="docs/images/texturequest-banner.png" alt="TextureQuest — an EverQuest II texture pack" width="100%"></p>

# TextureQuest

**More detail. The EverQuest II you remember.**

TextureQuest is a community texture pack for **EverQuest II Origins**. Its aim is to retain the game's original style, recognizable designs and art direction while bringing out the detail in armor, creatures, signs, buildings and terrain.

AI-assisted upscaling adds finer surface detail while keeping the familiar look of Norrath. The pack includes **24,693 textures**, mostly at **1K or 2K**, with selected large creatures and heavily stretched surfaces at **4K**. It covers equipment, characters, creatures and environments. The game's models, animations and lighting remain unchanged. Some fine detail is newly generated, and results vary by texture.

## Download and install

**[Download TextureQuest 1.4.2 for Windows](https://github.com/Maergoth/TextureQuest/releases/download/v1.4.2/TextureQuest-Installer-1.4.2-windows-x64.zip)** · [Latest release and downloads](https://github.com/Maergoth/TextureQuest/releases/latest)

App **1.4.2** offers two installation modes, with a hover info icon explaining each option. **Replace PAK files is recommended because it avoids injecting a runtime component into the game process.** Choose one mode at a time; an existing background-mode selection is retained.

| Mode | How it works and advantages | Tradeoffs |
| --- | --- | --- |
| **Replace PAK files — recommended** | Copies verified replacement archives into the game's `paks` folder. The game loads them directly, so no background watcher or injected loader is needed. | Changes the game archives. A game patch can overwrite them, and returning to stock requires the official patcher's file validation. |
| **Background texture loader** | Builds a private cache from stock game files and verified replacements. A per-user startup watcher detects the selected **EverQuest2.exe** and loads a DLL that redirects selected file reads inside the game. The game's PAKs, settings and shortcuts stay unchanged. | Needs the portable app, its watcher and private cache to remain available. It injects a runtime component, has limited client coverage, and changed game archives or indexes require a manual cache refresh. Attached clients retain the loader until they close. |

The background loader's first Windows Origins in-world test succeeded; compatibility with other client versions and every possible launch path has not been established. Hover the info icon beside either mode to review its behavior and tradeoffs before installing.

The texture pack remains **1.2.3**, with the same **24,693 textures across 310 archives** and unchanged archive bytes and download URLs. This includes **312 recovered 2K textures**: 137 from the Maj'Dul recovery and 175 other Desert of Flames textures, plus the earlier missed textures, effects, alpha maps, Ratonga skin repair and selected 4K updates. Additional missed-texture generation is unfinished and is not included in this release.

1. **Patch the game first**, then close every EQ2 client and the launcher. If TextureQuest PAK replacements are installed and you want background mode, run the official patcher's file validation to restore the stock game archives first.
2. Extract the installer ZIP into a writable folder outside the game. Keep its contents together and open **TextureQuest.exe**.
3. Choose the game folder containing `EverQuest2.exe` and `paks`. Use **Download / Resume** to fetch and verify the required texture archives. Interrupted downloads resume, and unchanged saved downloads are reused. No separate texture ZIP, Python, AI software or GPU generation is needed.
4. Select **Replace PAK files** (recommended) or **Background texture loader**; hover its info icon for details. With the game and launcher closed, click **Install TextureQuest** in Replace mode or **Install background loader** in Background mode. PAK mode copies the verified pack into the game. Background mode checks for stock archives, prepares its private cache and installs the startup watcher.
5. Wait until background installation reports ready before launching the game normally through your usual EXE, launcher or shortcut. Keep the extracted app and its `data` folder in place. **Launch Game** also starts the selected EverQuest2.exe directly.

**Background loader removal:** use the app's background-loader uninstall action. Uninstall stops the startup watcher and future attachments. A client already attached to the loader retains it until that client closes. To change installation modes, uninstall the current background loader and close all active clients first. Restore stock files with the official patcher before installing background mode over an earlier PAK installation.

**After a game patch:** close the game and launcher. PAK replacement may need reinstalling. If a patch changes game archives or indexes, uninstall and reinstall the background loader to refresh its private cache. Automatic post-patch cache refresh is unfinished. An EXE-only update does not itself change texture content, but compatibility with a new client build still needs validation. Do not skip required game updates.

The app keeps verified ready-to-use texture archives under `data/downloads` and reuses them for either mode. Background mode also needs space for its private cache. The app shows download size, checks space as it works and resumes interrupted downloads. Keep the app's `data` folder to retain downloads and installation history. Installation starts only when you click Install.

The app checks **texture updates and app updates separately** on startup. Use **Settings → Check for updates** to check again. New textures use **Download / Resume**, followed by a separate explicit installation. **Get app update** downloads and verifies the new app, replaces its managed files and restarts TextureQuest while preserving `data`. In app **1.4.2**, an available **Get app update** button remains clickable when the background loader is installed. It explains the prerequisite and asks whether to uninstall the owned loader and continue. Close all attached EQ2 clients first; active sessions or unknown ownership prevent the update. **Install the background loader again after the app update.** App 1.4.1 and earlier may require uninstalling the loader manually before Get app update becomes available. Before clearing the app cache, uninstall the loader and close all attached clients.

**One manual upgrade is needed from app 1.3.4 or earlier:** close the old copy, extract app 1.4.2 into a new writable folder and move your existing `data` folder alongside the new `TextureQuest.exe`. Those older apps open the release page rather than updating themselves. Apps 1.3.5 and later support **Get app update**. Keep the app outside the game folder.

**To return to stock:** uninstall the background loader and close any attached clients. If PAK replacement was used, run the official patcher's file validation to restore the original game files.

**Compatibility:** this pack was developed for EverQuest II Origins. The background loader has an initial successful test on one exact Windows Origins client; other EQ2 versions, future content patches and all launch paths have not been fully tested. Some textures are intentionally left unchanged for compatibility. This app release does not change the game's GPU settings or the separate generation model.

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
