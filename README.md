<p align="center"><img src="docs/images/texturequest-banner.png" alt="TextureQuest — an EverQuest II texture pack" width="100%"></p>

# TextureQuest

**More detail. The EverQuest II you remember.**

TextureQuest is a community texture pack for **EverQuest II Origins**. Its aim is to retain the game's original style, recognizable designs and art direction while bringing out the detail in armor, creatures, signs, buildings and terrain.

The pack uses AI-assisted upscaling with targeted corrections for texture artifacts, color, terrain repetition and character seams. It keeps the game's existing models, texture layout and material structure. It is a reinterpretation of low-resolution artwork: some fine detail is newly generated, and results vary by texture.

## Download and install

**[Download the Windows installer](https://github.com/Maergoth/TextureQuest/releases/download/v1.0.0/TextureQuest-Installer-1.0.0-windows-x64.zip)** · [Release notes and files](https://github.com/Maergoth/TextureQuest/releases/tag/v1.0.0)

1. **Patch the game first**, then close every EQ2 client and the launcher.
2. Extract the installer ZIP into a writable folder outside the game. Keep its contents together and open **TextureQuest.exe**.
3. Download the pack through the app. Interrupted downloads can be resumed. No Python, AI software or GPU generation is needed.
4. Select the game folder containing `EverQuest2.exe` and `paks`, then click **Install**. Wait for verification to finish.
5. For normal play, launch **EverQuest2.exe directly**, or make a shortcut to it. Opening the launcher can replace the modified textures.

The full pack downloads **27.8 GB**. Allow roughly **56 GB of free working space**, in addition to your game installation, for the downloaded ZIP and prepared archives. Keep the downloaded pack for reinstalling after patches. The installer does not make original-game backups or install anything until you click Install.

**After a game patch:** use the app's **Uninstall** action to unlock managed files, run the official patcher, close it, and install TextureQuest again. Do not skip required game updates. The read-only file attribute is not protection against the patcher.

**To return to stock:** click **Uninstall**, then run the official patcher. Uninstall removes the installer's file protection; the patcher restores the original game files.

Already have the verified `Texture-Pack.zip`? Use the installer's local pack selection, or extract the ZIP's VPK files directly into the game's `paks` folder with the game and launcher closed. These are complete replacement archives, retaining untouched entries and previous upscales within each archive.

## What's in 1.0

| Item | Included |
| --- | --- |
| Upscaled textures | 23,277 |
| Complete replacement archives | 310 |
| Target resolution | 1K or 2K, preserving rectangular aspect ratios |
| Character corrections | Approved human/Erudite seams, vampire repairs and Ratonga eye-socket v2 |
| Other finishing | Nightblood color repair; selected terrain, broad-color and digital-pattern corrections |
| Final color pass | +15% contrast, then +15% saturation, applied once |

Known control textures, compositor inputs, unsupported formats and other protected entries remain stock. This is a texture replacement pack, not a model, animation or lighting overhaul. Coverage is based on the audited Origins texture set; this release has not been validated for every EQ2 installation or later content update.

## Close-up comparisons

Comparisons show original game textures and TextureQuest on the same geometry, with matching camera and preview lighting. Model previews approximate the game renderer; actual game lighting, character customization, animation and material effects can look different. Comparison images and their provenance are included with the release documentation.

<!-- GALLERY: authentic shipped-pack comparisons added before publication. -->

## Limitations

- **Seams and tiling can become more visible.** The original low-resolution textures softened or concealed some joins, repeated patterns and mismatched material boundaries. More detail can expose them. Targeted repairs reduce specific problems; they do not eliminate every seam or repeated tile.
- **AI can get details wrong.** Small lettering, symbols, eyes and tightly packed texture regions are particularly difficult. Some source information simply does not exist at higher resolution.
- **The original geometry still matters.** Low-polygon silhouettes, UV stretching, separate body parts and LOD changes remain. Texture work cannot fix every model or shader issue.
- **Not every appearance was reviewed in game.** Some character variants, morphs and layered materials may still need corrections. Approved comparison previews are useful, but are not a guarantee for every in-game situation.
- **Higher-resolution textures use more resources.** Storage, loading behavior and texture memory requirements can increase. Performance depends on your settings and system; no minimum hardware or FPS improvement is claimed.
- **Game patches can overwrite the pack.** Reinstall after patching, and use the game executable directly for ordinary play.

Please [report a problem](https://github.com/Maergoth/TextureQuest/issues/new/choose) with the game version/server, zone or item/NPC, character race/gender/appearance where relevant, a close-up screenshot, and the TextureQuest version installed. Include whether **Verify installation** passes. Do not include account credentials or private logs.

## Updates after 1.0

Each version has its own GitHub release, notes, installer and checksums. Released files are versioned; a new version will not silently change your installed textures.

When only a few textures change, an update can distribute only the affected **complete PAKs**, preserving the other TextureQuest textures inside each one. A full pack remains available for fresh installations. Any smaller update will state its required base version and installation order explicitly; 1.0 is a complete installation, not an update bundle.

The installer opens the [latest release page](https://github.com/Maergoth/TextureQuest/releases/latest) when checking for updates. It does not update or install a new pack without your action. A game patch and a TextureQuest update are separate: reapplying your existing pack after patching does not require downloading it again.

## Manual downloads and integrity

GitHub limits individual release assets to under 2 GiB, so the full pack is distributed as 14 ordered parts. The installer downloads and verifies them and joins them into the **exact original ZIP**. These are raw pieces of one ZIP; individual parts cannot be extracted by themselves. Do not download GitHub's automatically generated **Source code (zip)** expecting the texture pack.

The resulting `Texture-Pack.zip` is **27,763,745,032 bytes** with SHA-256:

```text
9501071759d57c26175ee9245b3f5dfee4d4cee7f19a57c039289d09d2041caf
```

The release manifest contains each part's hash and the archive inventory. [GitHub's release storage policy](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases#storage-and-bandwidth-quotas) explains the per-file limit.

## Credits and ownership

TextureQuest is an unofficial community project and is not affiliated with or endorsed by the EverQuest II developers or publisher. The original game artwork, names and trademarks belong to their respective owners. Distribution of this pack does not grant ownership of or relicense the underlying game assets. Third-party software notices are included with the installer.
