<p align="center"><img src="docs/images/texturequest-banner.png" alt="TextureQuest — an EverQuest II texture pack" width="100%"></p>

# TextureQuest

**More detail. The EverQuest II you remember.**

TextureQuest is a community texture pack for **EverQuest II Origins**. Its aim is to retain the game's original style, recognizable designs and art direction while bringing out the detail in armor, creatures, signs, buildings and terrain.

AI-assisted upscaling adds finer surface detail while keeping the familiar look of Norrath. The pack includes more than **23,000 textures at 1K or 2K**, covering equipment, characters, creatures and environments. The game's models, animations and lighting remain unchanged. Some fine detail is newly generated, and results vary by texture.

## Download and install

**[Download the Windows installer from the latest release](https://github.com/Maergoth/TextureQuest/releases/latest)**

1. **Patch the game first**, then close every EQ2 client and the launcher.
2. Extract the installer ZIP into a writable folder outside the game. Keep its contents together and open **TextureQuest.exe**.
3. Choose the game folder containing `EverQuest2.exe` and `paks`, then download the pack through the app. Interrupted downloads can be resumed. No Python, AI software or GPU generation is needed.
4. Once the pack is ready, click **Install TextureQuest**. Wait for verification to finish.
5. For normal play, launch **EverQuest2.exe directly**, or make a shortcut to it. Opening the launcher can replace the modified textures.

The 1.0 base download is **27.8 GB**; newer releases may include an additional update. Allow room for downloaded ZIPs, a complete prepared pack and installation staging. The base alone needs about **72 GB free if everything is on one drive**, and updates or retained older caches need additional space. The app checks space as it works. Keep downloaded files for reinstalling after patches. The installer does not make original-game backups or install anything until you click Install.

**For a game patch:** open **Maintenance & updates → Prepare for a game patch**, run the official patcher, close it, and install TextureQuest again. Do not skip required game updates. The read-only file attribute is not protection against the patcher.

**To return to stock:** choose **Maintenance & updates → Return to original textures**, then run the official patcher. The app removes its file protection; the patcher restores the original game files.

Already have the full `Texture-Pack.zip`? Choose **Maintenance & updates → Use an existing Texture-Pack.zip**. The installer for an updated release can reuse verified files and fetch its additional PAKs. A fresh installation gets the required base files automatically. Update-only downloads are not a substitute for the full pack; use the installer to assemble the correct set.

**Compatibility:** this pack was developed for EverQuest II Origins. Other EQ2 versions and later content updates have not been fully tested. Some textures are intentionally left unchanged for compatibility.

## Examples

Close-ups of older Norrath, using the original game models and their material layers with matching cameras and lighting. Original textures are on the left; the released TextureQuest pack is on the right. These are 3D previews; appearance in game will vary with lighting, settings and character customization.

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

**Hell Boar — fur, skin and tusks**

![Stock and TextureQuest Hell Boar](docs/images/monster-hell-boar.png)

**Flesh Golem — face, skin and stitching**

![Stock and TextureQuest Flesh Golem](docs/images/monster-flesh-golem.png)

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
- **Game patches can overwrite the pack.** Reinstall after patching, and use the game executable directly for ordinary play.

Please [report a problem](https://github.com/Maergoth/TextureQuest/issues/new/choose) with the game version/server, zone or item/NPC, character race/gender/appearance where relevant, a close-up screenshot, and the TextureQuest version installed. Include whether **Verify installed textures** passes. Do not include account credentials or private logs.

## Updates

Choose **Maintenance & updates → Check for updates** to open the [latest release page](https://github.com/Maergoth/TextureQuest/releases/latest). Follow that release's installation instructions. Updates are never installed automatically.

After a game patch, you can reapply your downloaded pack without downloading it again.

### Can this leave the game's PAKs untouched?

The supported installer replaces PAK files. **Sideload** is an experimental option under development and is currently unavailable. A texture-injection tool would need to intercept rendering and reliably identify the game's textures; ordinary ReShade color presets cannot load this pack. No injection method has been validated for this release.

We have no confirmation from Daybreak approving either approach. The 1.1 installer displays this warning:

> We don't know what's more legal, replacing the game files or hooking the process. If Daybreak hates you, they might ban you. Be warned.

### Can I choose only races, armor, weapons or an expansion?

The installer installs the complete pack. Separate content selections are not available because the game's archives mix equipment, characters and shared world textures.

## Technical details and contributing

The [technical guide](docs/TECHNICAL.md) covers the model and generation recipe, resolution selection, material and DDS compatibility findings, corrections, validation, and building the installer. It also explains what is available to reproduce from this repository.

For manual downloads and checksums, use the [release files](https://github.com/Maergoth/TextureQuest/releases/tag/v1.0.0). The installer handles the fourteen pack parts automatically. GitHub's **Source code (zip)** does not contain the textures.

## Credits and ownership

TextureQuest is an unofficial community project and is not affiliated with or endorsed by the EverQuest II developers or publisher. The original game artwork, names and trademarks belong to their respective owners. Distribution of this pack does not grant ownership of or relicense the underlying game assets. Third-party software notices are included with the installer.
