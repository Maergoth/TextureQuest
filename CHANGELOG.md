## 1.7.2 — texture catalog 1.2.4

Fae/Arasai body and Qeynos stone-trim corrections, including the paired map. Graphics runtime unchanged. Other released textures preserved.

# Changelog

## 1.7.1

- Fix Graphics hook installation failing with "output must be inside the loader component".
- Keep offline pack preparation inside its owned component; retain separate graphics cache and ownership receipts.
- Add fresh-install integration coverage through the bundled preparer and READY verification.
- Graphics binaries and texture pack unchanged.

## 1.7.0

- Normal game-folder graphics proxy; no external graphics injector or watcher.
- On-demand hotkey switching, configurable Numpad * default.
- Updated installer help, legacy migration and owned-file uninstall.
- Publish modified provider source/build overlay; texture pack unchanged.

## 1.6.0

- Two install choices: Graphics hook or Replace PAK files.
- Remove Dual-load option; new installs use on-demand comparisons.
- Keep an uninstall-only path for old Memory hook installations.

## 1.5.1

- Shorter installer/README help: automatic startup after reboot, normal launches, folder retention and uninstall behavior.
- Wording only; runtime and texture pack unchanged.

## 1.5.0

- Full-pack Graphics hook, now the default, with Original/Upscaled switching.
- Customizable foreground shortcut, default Numpad *.
- Optional bounded Dual-load comparison cache and on-demand mode.
- Three exclusive install modes with updated hover help.
- Texture pack 1.2.3 and all released archive bytes/URLs unchanged.

## 1.3.2

- Overall download/install percentage, bytes and PAK count; current-file status remains visible.
- Includes the fast streamed verification/copy path without per-texture decompression or repeated finished-file scans.
- Texture1.2.0 and all310 existing public VPK identities/URLs remain unchanged.

## 1.1.1

- The installer checks the latest published texture release in the app and before downloads.
- Downloads are saved and reused automatically; no existing texture ZIP is required.
- Installed archives remain writable. After a game patch, rerun the installer.
- Identical prepared packs are reused across installer-only updates without another full archive copy.
- Texture content is unchanged from 1.1.0.

## 1.1.0

- Forty selected textures at a 4K longest edge, including the Leviathan body and related creatures, dragons, armor and stretched terrain. Rectangular proportions are preserved.
- Complete updated PAKs retain all other textures and existing character repairs.
- The new installer reuses verified unchanged archives and downloads the required update. Fresh installations obtain the existing base automatically.
- Sideload is displayed as unavailable while a compatible texture-injection backend is investigated.

See the latest GitHub release for the available installer.

## 1.0.0

Initial complete TextureQuest release for EverQuest II Origins.

- 23,277 upscaled textures across 310 complete replacement archives.
- Resolution selected at 1K or 2K, with rectangular texture proportions retained.
- Pattern-artifact correction, selected terrain edge/broad-color work, and additional color/overlay coverage.
- Approved human/Erudite seam, vampire, Nightblood and Ratonga eye-socket v2 repairs.
- One +15% contrast and +15% saturation finish.
- Verified source DDS formats, alpha/mips and protected stock entries in the build pipeline.
- Original PNG generations preserved during pack-only finishing.

Known limitations are described in the README. This release does not claim every seam, appearance or in-game artifact is fixed.
