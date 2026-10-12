<p align="center"><img src="docs/images/texturequest-banner.png" alt="TextureQuest — an EverQuest II texture pack" width="100%"></p>

# TextureQuest

**More detail. The EverQuest II you remember.**

TextureQuest is a community texture pack for **EverQuest II Origins**. Its aim is to retain the game's original style, recognizable designs and art direction while bringing out the detail in armor, creatures, signs, buildings and terrain.

AI-assisted upscaling adds finer surface detail while keeping the familiar look of Norrath. The pack includes more than **23,000 textures at 1K or 2K**, covering equipment, characters, creatures and environments. The game's models, animations and lighting remain unchanged. Some fine detail is newly generated, and results vary by texture.

## Download and install

**[Download the Windows installer](https://github.com/Maergoth/TextureQuest/releases/download/v1.0.0/TextureQuest-Installer-1.0.0-windows-x64.zip)** · [Release notes and files](https://github.com/Maergoth/TextureQuest/releases/tag/v1.0.0)

1. **Patch the game first**, then close every EQ2 client and the launcher.
2. Extract the installer ZIP into a writable folder outside the game. Keep its contents together and open **TextureQuest.exe**.
3. Choose the game folder containing `EverQuest2.exe` and `paks`, then download the pack through the app. Interrupted downloads can be resumed. No Python, AI software or GPU generation is needed.
4. Once the pack is ready, click **Install TextureQuest**. Wait for verification to finish.
5. For normal play, launch **EverQuest2.exe directly**, or make a shortcut to it. Opening the launcher can replace the modified textures.

The full pack downloads **27.8 GB**. Allow about **72 GB free if everything is on one drive**: roughly 56 GB for the downloaded ZIP and prepared archives, plus 16 GB for the game's larger replacement files and installation staging. With separate drives, reserve 56 GB on the installer drive and 16 GB on the game drive. Keep the downloaded pack for reinstalling after patches. The installer does not make original-game backups or install anything until you click Install.

**For a game patch:** open **Maintenance & updates → Prepare for a game patch**, run the official patcher, close it, and install TextureQuest again. Do not skip required game updates. The read-only file attribute is not protection against the patcher.

**To return to stock:** choose **Maintenance & updates → Return to original textures**, then run the official patcher. The app removes its file protection; the patcher restores the original game files.

Already have `Texture-Pack.zip`? Choose **Maintenance & updates → Use an existing Texture-Pack.zip**, or extract the ZIP's VPK files directly into the game's `paks` folder with the game and launcher closed. Each included archive is a complete replacement, so you do not need to install earlier TextureQuest versions first.

**Compatibility:** this pack was developed for EverQuest II Origins. Other EQ2 versions and later content updates have not been fully tested. Some textures are intentionally left unchanged for compatibility.

## Close-up comparisons

Original textures and TextureQuest are shown on the same game models with matching cameras and lighting. These are 3D previews; appearance in game will vary with lighting, settings and character customization.

**Armor — scales and metal trim**

![Stock and TextureQuest Mermaid scale armor](docs/images/armor-mermaid.png)

**Monsters — Hell Boar fur, skin and tusks**

![Stock and TextureQuest Hell Boar](docs/images/monster-hell-boar.png)

**Signage — the bakery sign**

![Stock and TextureQuest bakery sign](docs/images/signage-bakery.png)

**Wood and metal — a Freeport door**

![Stock and TextureQuest Freeport wooden door](docs/images/wooden-door.png)

**Terrain — Darklight Woods grass**

![Stock and TextureQuest Darklight Woods terrain](docs/images/terrain-darklight.png)

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

### Can I choose only races, armor, weapons or an expansion?

Version 1.0 installs the complete pack. Separate content selections are not available because the game's archives mix equipment, characters and shared world textures.

## Manual downloads and integrity

The installer handles the download and checksums for you. If downloading manually, the pack consists of **14 ordered parts** that must be joined into one `Texture-Pack.zip` before extraction. Individual parts cannot be extracted on their own. GitHub's **Source code (zip)** does not contain the textures.

The resulting `Texture-Pack.zip` is **27,763,745,032 bytes** with SHA-256:

```text
9501071759d57c26175ee9245b3f5dfee4d4cee7f19a57c039289d09d2041caf
```

Individual file checksums are available with the [release downloads](https://github.com/Maergoth/TextureQuest/releases/tag/v1.0.0).

## Credits and ownership

TextureQuest is an unofficial community project and is not affiliated with or endorsed by the EverQuest II developers or publisher. The original game artwork, names and trademarks belong to their respective owners. Distribution of this pack does not grant ownership of or relicense the underlying game assets. Third-party software notices are included with the installer.
