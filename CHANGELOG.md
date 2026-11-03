# Changelog

## 1.13.8 — graphics candidate

- Increase complete particle capture capacity to 512 draws / 32,768 triangles for the captured combat workload. Add independent particle-light strength and radius controls.
- Raise emissive and particle light ceilings to 512 and 64, retaining absent defaults of 64 and 4. Shadow maps above 256 allocate on demand and can add 384 MiB of GPU memory.
- Apply Painted emission kept as a constant share on strictly qualified native world passes, independent of optional emitter measurement and the camera-dependent light mask.
- Increase the supported material-normal draw cache to 512 and allow better material records to replace base records even when full.
- Preserve an original keyboard hook's handled F10 result, addressing the demonstrated redundant Windows menu activation path. Keep Home and existing Alt handling.
- Reconcile the selected Graphical Effects group with all available bundles during normal refresh, including legacy effects-only selections that omitted the DLSS panel.
- Preserve texture data 1.3.0 and the AIO/DLSS native payloads. Actual game behavior, complete beam coverage and FPS remain unproved; see the 1.13.8 handoff. This candidate is not a new public release.

## 1.13.7

- Reuse the committed texture preparation during app and graphics-hook updates when the texture version and preparation identity are unchanged. Avoid rescanning every DDS file during setup, hook activation and cache cleanup.
- Keep full verification for a new texture version and the explicit Verify / Update textures command. Changed saved metadata requests repair before the installed hook is replaced.
- Graphics binaries and texture data remain unchanged from 1.13.6.

## 1.13.6

- Keep frame generation off when its native setting is missing, as well as in the installer defaults. Preserve explicit saved choices.
- Give both shadow-softness controls a 1–10 display range while retaining existing native values and appearance until adjusted.
- Restore the upstream material-colour saturation control, with independent demand tracking and the common Original toggle.
- Stop finite distant or oversized particle primitives from cancelling nearby emissive sources in the same sample. Retain strict rejection of malformed or incomplete samples.
- Add Advanced emissive and particle light budgets. Defaults remain 64 and 4; both share the global Shadowed lights pool.
- Correct sun-filter map-edge sampling and invalid reduced-shadow neighbours. Extend bounded frame-capture diagnostics for the remaining sun-direction flicker.
- Continue experimental graphics fixes with source and controlled-test limits documented for the maintainer handoff. Texture data remains 1.3.0.

## 1.13.5

- Measure supported particle materials per triangle and accumulate their light in world space, replacing screen-coverage normalization and transient draw-batch tracking. Preserve animated fire.
- Require current native cascade/caster evidence for the sun-direction correction and fade changes instead of using the abrupt timeout.
- Merge compatible upstream window projection, lamp fade, loading-scene reset and distance-consistent sun filtering fixes.
- Preserve saved controls, native bloom choices, texture/DLSS/Reflex functionality and the normal launcher update flow. Local preview; visual stability and performance still need an in-game check.
- Use the selected graphics defaults for absent settings while retaining existing custom values, legacy off choices and conditional uninstall restoration.

## 1.13.4 — private graphics preview

- Combine supported static and painted emissive surfaces with captured spell-particle lighting under one bounded light budget.
- Correct particle sample-expiry and pending-readback timing gaps; fade disappearing sources briefly instead of dropping them immediately.
- Apply compatible upstream transparent/small-caster cache fixes. Preserve the existing private window-shadow path and saved settings.
- Experimental local candidate; complete particle coverage, visual stability and performance remain in-game checks.

## 1.8.0

- Graphics hook is now an add-on for stock ReShade; no modified ReShade or private texture cache.
- Upscaled textures load at creation from the downloaded archives using a published texture map.
- Numpad * comparison keeps working; Original loads stock copies on demand.
- Existing ReShade installs are never changed; the add-on build is used as is, and a standard build stops installation with an explanation.


## 1.7.2 — texture catalog 1.2.4

Fae/Arasai body and Qeynos stone-trim corrections, including the paired map. Graphics runtime unchanged. Other released textures preserved.

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
