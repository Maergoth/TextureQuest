# Changelog

This is the canonical history bundled with the installer. Develop entries are
development builds, not announcements of public binary releases. Controlled
checks described here do not establish complete in-game appearance or FPS.

## 1.13.20 — Local particle-shadow motion candidate (not published)

- Let particle shadows follow smaller movements of the already-smoothed light source. Previously, a shadow map could hold its old position until the source moved 0.1 world units, hiding much of that smoothing and then jumping.
- Add a bounded allowance for finer particle-shadow updates after normal required updates. Reuse conservative particle caster lists while valid to reduce repeated CPU searches. This trades extra shadow rendering for smoother motion; crowded scenes and source reassignment can still step.
- Keep the failed 150 ms shadow fade removed. A controlled software-rendered scene shows smaller shadow-position errors and jumps, but actual EQ2 appearance and GPU cost still need comparison.
- Retain the shared Low/Medium/High/Max presets, 16x anisotropic filtering, existing custom settings and textures 1.3.0. No texture re-download or unchanged DDS/map scan is required.

## 1.13.19 — Lighting quality presets

- Promote the qualified object/particle source observations, independent per-light shadow visibility and conservative receiver-tile lighting work from the local comparison builds. Remove the comparison checkbox. Retain optional surface-light smoothing under Advanced.
- Revert the 150 ms shadow crossfade from 1.13.18 after the in-game result did not improve the reported shadow jumping. Remove its retained-map memory and blending work. Particle grouping and the existing source-position smoothing remain; this release does not claim to eliminate every grouping or shadow-map step.
- Add shared **Low, Medium, High, Max** quality presets to both the installer and the top of the TextureQuest panel. Use the same versioned preset contract for both. Manual changes show Custom; selecting a preset explicitly applies its complete lighting profile without changing Frame Gen or Neural Rendering.
- Budget Low around an RTX 2060, Medium around an RTX 3070, High around an RTX 4080 and Max around an RTX 5090. Max enables the highest supported light/shadow capacities. These are starting points, not measured hardware FPS guarantees; artistic light strength, radius and shadow softness are kept consistent across tiers.
- Keep **Show FPS**, **Frame Gen** and **Quality Presets** at the top. Move individual graphics controls to **Advanced** and diagnostics, captures and performance logging to **Debug**. Remove experimental labels from active controls. Keep anisotropic filtering on at 16x in every preset.
- Preserve textures 1.3.0, portable data, saved custom settings and unchanged texture preparation. Frame Gen and Neural Rendering stay off on a fresh install and remain independent of quality presets. Upscaling and Reflex driver mode remain retired.

## 1.13.18 — Local experimental shadow transitions (not published)

- Rename the existing Advanced comparison checkbox to **Experimental improvements**. It still starts unchecked each launch, preserves saved graphics preferences, and retains the same current/candidate comparison and opt-in CSV logging.
- Blend compatible old/current emissive-shadow endpoints over 150 ms when the same light keeps its owner and shadow-pool slot. Each endpoint pairs its static map with the moving-caster snapshot and source position/metadata actually used. Keep current animated light colour rather than fading old emission colour.
- Require valid same-owner/same-slot history. Ranking or slot turnover remains current-only; missing, partial or incompatible history uses valid current shadowing. Reassignment, invalid history and settings/epoch/zone changes abort the fade. A frozen moving silhouette can briefly trail or double during the 150 ms blend. Retain normal map updates, resolution, filtering and limits; six logical transitions share lazy history pools capped at 84 MiB.
- Add `transition_lights` to comparison CSV, `mean_transition_lights` to summaries and a mean transitioning-lights row in the UI. Retain the 1.13.17 candidate's qualified object/particle source observations and independent visibility. The unchecked path remains current 1.13.16 lighting.
- This is a private comparison candidate. Controlled validation and final renderer provenance are required before packaging; live EQ2 appearance and hardware performance remain to be compared. Texture data remains 1.3.0, with saved settings and unchanged texture preparation reused by the normal updater.

## 1.13.17 — Local lighting comparison candidate (not published)

- Add **Home → TextureQuest → Advanced → Test lighting improvements**. Unchecked retains the current 1.13.16 renderer; checked selects the candidate. Changes commit at frame boundaries and rebuild source caches. The experiment starts off each launch and does not overwrite saved graphics preferences.
- Add opt-in **Record comparison performance (CSV)**, a current/candidate table, and a summary file on stop. Record asynchronous GPU lighting times, game Present intervals, light counts, resolution, settings identity, warm-up and fallback markers. Exclude at least three seconds and 180 frames after switches from summaries. GPU lighting covers list construction, lighting and reconstruction; it excludes emitter measurement and shadow-map rendering.
- For two verified particle vertex profiles, measure source geometry before the native camera-distance size clamp. Preserve native animated material colour, UVs, alpha behavior and light retention. Unsupported profiles keep their existing measurement path.
- For one verified painted-object shader pair, sample complete material triangles using the native material shader and stable source anchors. Preserve alpha discard and source animation while avoiding camera-visible colour averages. Unsupported material modes retain their existing path.
- Give each admitted light its own shadow visibility instead of borrowing it from other lights. Conservatively bound negligible shadow contributions over receiver tiles; small light sets use the exact independent path. Keep shadow resolution and sampling unchanged.
- Controlled native-shader/software Direct3D and CPU checks cover source invariance, contribution bounds, state restoration and logging. They do not establish game FPS or resolve every native source. Shadow-sampling replacements remain research; no Tensor Core runtime is added. Texture data remains 1.3.0.

## 1.13.16 â€” Local lighting candidate (not published)

- Keep object-emission measurements attached to observed world cells. Unknown areas no longer inherit a newly visible colour from a parent or a whole-object average. Combine known energy consistently across lighting hierarchy levels, retain it off screen, and reject older readbacks after a newer result.
- Wait for the emitter's geometry before sampling, avoiding an initial measurement that cannot be placed in the world. A sparse first observation may initialize a local cell; established cells retain their colour until a measurement has enough support. Correct tiny, unambiguous grid-boundary reprojection errors without borrowing colour across unknown areas. Particle animation and the existing retention interval stay unchanged.
- Avoid static shadow submissions to cube faces that a mesh's bounds cannot intersect. Preserve map resolution, filtering, moving-object updates and all light/shadow budgets.
- Reject invalid or unrelated-surface samples during emissive-light reconstruction. Add **Advanced â†’ Smooth emissive lighting (experimental)** as a separately compiled, optional surface-lighting filter. It starts off because it costs extra shader work; texture colour and frame history are not filtered.
- Controlled CPU and software Direct3D checks cover energy conservation, culling and reconstruction. This is a local test candidate: actual EQ2 appearance, the reported camera-motion flicker and hardware FPS still require comparison. Unknown native emitters and partial observations within one world cell remain limitations.

## 1.13.15 â€” Graphics release

- Bring the Develop particle grouping, retained-source, material-normal replay and bounded self-shadow work into the public installer. Texture data remains 1.3.0; no unfinished generations are included.
- Show **Graphical Effects / DLSS (experimental)** in the launcher and clearly explain that **Home** shows or hides settings.
- Default ambient occlusion on. Default frame generation, neural rendering, smooth interface text, point lights, emissive lights, sun lighting and sun shadow softening off. Preserve saved choices and numeric customizations for retained features.
- Remove Upscaling and Reflex driver mode from the controls and active runtime paths. Ignore legacy SR enable requests; retain the binary API layout for compatibility.
- Add **Low, Med, Max** Shadow quality and Light quality presets, including particle shadow/light budgets. Keep custom numeric controls under Advanced and explain the shared shadow-pool limit.
- Version the installer's default-settings leases so old installations can still restore their exact owned settings without adopting the new defaults.
- Sun-direction movement flicker, unsupported material/particle paths and sources the game never submits remain limitations. The latest source-core visual improvement is scene-dependent; no universal appearance or FPS improvement is claimed.

## 1.13.14 â€” Develop graphics candidate

- Add a bounded, particle-only exclusion core to shadow depth sampling. Its radius is `min(0.35, 0.5 Ã— weighted positive-emission centroid RMS)` world units, with smoothed source metadata.
- Keep built light position and exclusion core together across static and moving shadow maps, so each path uses the core that produced the static map.
- This excludes nearby cloud occluders spatially; it does not identify the emitter's owner or apply a global shadow bias. Unrelated geometry inside the small core can also be ignored; blockers outside it remain eligible.
- Pass 1,381 focused CPU checks, a strict native build and 35 controlled hardware checks. Seven other native payloads, texture data 1.3.0 and saved controls are retained. Actual EQ2 appearance and performance still need comparison; this is not a public binary release or a universal self-shadow fix.

## 1.13.13 â€” Develop app update

- Add **What's new** inside the installer, showing this bundled history with the installed app version clearly identified.
- Bundle and pin the same canonical changelog used on GitHub. Require an entry for the app's current version when building the installer.
- Backfill the 1.13.10â€“1.13.12 graphics history and document how developers keep future entries current.
- App-only update: retain the 1.13.12 renderer, all other native payloads and texture data 1.3.0. No new native graphics fix or public binary release is claimed by this entry.

## 1.13.12 â€” Develop graphics update

- Replace centroid-grid particle identities with deterministic world-anchor association and ownership hysteresis, reducing identity changes at grid boundaries.
- Filter the representative light position over time while retaining current RGB energy, exact observed moments and native dark pulses. Commit grouping, history and field state together.
- Preserve the existing two-second hold, one-second fade and service-gap behavior. Shadow maps keep their 0.1-unit movement threshold and normal rebuild scheduler; continuous shadow updates are not claimed.
- Pass a strict native build and 1,271 focused CPU checks. Fixture timings are not game FPS; ambiguous source histories, native culling and costly adversarial cases remain limitations. Seven other native payloads and texture data 1.3.0 are unchanged.

## 1.13.11 â€” Develop graphics update

- Preserve committed particle light and map identities when allocation service is unavailable, while still honoring real zero budgets, fade, expiry, reduced limits, zero strength, Off and scene resets.
- Group nearby particle contributions before selection to reduce duplicated fine-cell lights. The fixed voxel identity rule introduced here is replaced by stable association in 1.13.12.
- Restore weighted T0 material-normal replay for two verified opaque native base shader pairs, retaining smooth basis interpolation. Unknown, alpha-tested and unsupported paths keep geometric fallback.
- Pass focused production CPU checks and a targeted hardware normal-replay case. These checks do not establish every model seam, offscreen effect or FPS; unchanged hardware suites were not replayed. Other seven native payloads and texture data 1.3.0 remain unchanged.

## 1.13.10 â€” Develop graphics update

- Add a dedicated particle character-shadow pool, independent of the nearest 16 native moving-shadow slots. **Advanced â†’ Particle shadow limit** selects up to 128 eligible particle lights when **Character shadows** is enabled.
- Apply the existing shadow strength and softness controls to emissive shadows, preserving the saved legacy softness convention. The native 16 Ã— 512-pixel pool remains; the new 256-pixel banks allocate up to 192 MiB of depth storage.
- Retain previously observed particle cells for two seconds, then fade them for one second, in a bounded 256-entry cache. Held observations do not refresh their timestamps or create unseen sources.
- Temporarily use geometric fallback for base-pass normals instead of unverified replay; 1.13.11 restores the verified normal-mapped base profiles. Targeted hardware/cache checks passed, with live appearance and performance still requiring comparison.

## 1.13.9 â€” private Develop candidate

- Raise particle-light limit to 128, intensity ceiling to 128Ã—, and radius-control ceiling to 16Ã— / 128 world units.
- Keep later fitting particle draws when another draw exceeds bounded capture capacity or has unsupported data. Isolate per-draw failures while retaining global scene/epoch/readback failure guards.
- Correct particle WORLD shadow caster gathering and lookup. Moving character shadow maps retain nearest-16-light coverage; higher particle limits do not expand that coverage.
- Preserve saved controls, texture data 1.3.0 and the other seven native payloads. Focused controlled checks passed; full game appearance and FPS remain unproved. No public tag/release or main update.

## 1.13.8 â€” graphics candidate

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
- Give both shadow-softness controls a 1â€“10 display range while retaining existing native values and appearance until adjusted.
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

## 1.13.4 â€” private graphics preview

- Combine supported static and painted emissive surfaces with captured spell-particle lighting under one bounded light budget.
- Correct particle sample-expiry and pending-readback timing gaps; fade disappearing sources briefly instead of dropping them immediately.
- Apply compatible upstream transparent/small-caster cache fixes. Preserve the existing private window-shadow path and saved settings.
- Experimental local candidate; complete particle coverage, visual stability and performance remain in-game checks.

## 1.8.0

- Graphics hook is now an add-on for stock ReShade; no modified ReShade or private texture cache.
- Upscaled textures load at creation from the downloaded archives using a published texture map.
- Numpad * comparison keeps working; Original loads stock copies on demand.
- Existing ReShade installs are never changed; the add-on build is used as is, and a standard build stops installation with an explanation.


## 1.7.2 â€” texture catalog 1.2.4

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
