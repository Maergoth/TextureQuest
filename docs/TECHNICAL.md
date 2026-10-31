> App 1.8.0: the active Graphics hook add-on, hotkey and install lifecycle are documented in [GRAPHICS_HOOK.md](GRAPHICS_HOOK.md). Older app/release sections below describe historical implementations. Current Get app update downloads, verifies, replaces and restarts the app while preserving data.

# Technical guide

This guide records how TextureQuest 1.0 was made, the compatibility constraints discovered during development, and how to work on the public installer. For playing the game, start with the [installation instructions](../README.md#download-and-install).

## What is available to reproduce

This repository includes the Windows installer source and tests, release manifests, checksums, and comparison-image provenance. You can rebuild the installer and verify the released texture archives.

The full texture-generation, classification, mesh-analysis and repair toolchain is not included here. Neither are the original game archives, model weights, lossless generation masters or every per-texture processing record. The recipe below describes the production method; cloning this repository is not sufficient to regenerate the complete pack byte-for-byte. A seed and model name alone do not reproduce the selected revisions, source inputs or runtime.

## Generation recipe

| Setting | Production configuration |
| --- | --- |
| Model | [Qwen/Qwen-Image-2.1](https://huggingface.co/Qwen/Qwen-Image-2.1/tree/790c92633540aa0cb11d9abf19eb46d861714758) |
| Model revision | `790c92633540aa0cb11d9abf19eb46d861714758` |
| Pipeline | `QwenImage21Pipeline`, source-image-conditioned generation |
| Input | Native stock texture, with the pipeline's conditioning resize |
| Steps | 20 |
| Guidance | `true_cfg_scale=1.0` |
| Seed | 0 |
| Precision | BF16 |
| Negative prompt | None |
| Additional conditioning | No LoRA, ControlNet, material-description pass or generated anchor |
| Sampling | KV cache enabled; no cyclic sampling or periodic padding |
| Main output tiers | 1024 or 2048; rectangular images retain their proportions |

The later production prompt was:

> Upscale this with more detail. High resolution video game texture, photorealistic. Preserve smooth skin and facial surfaces where they are smooth in the source, with subtle natural detail.

Earlier completed generations retained the shorter prompt ending at “photorealistic.” The released pack combines those retained images, later generations and selected local repairs. It was not produced by applying one uniform prompt to every texture in a single run.

The isolated production runtime used Diffusers `0.41.0.dev0` and Transformers `5.17.0`. Development-version strings alone are insufficient runtime pins: exact library source, model files, processing code, input hashes and generation settings were recorded per frozen plan. Different hardware or library revisions can change results.

The 2048 path generated at the larger target from native stock, rather than enlarging a finished 1024 image. Its separate VAE decode used overlapping 1024-pixel tiles with a 768-pixel stride. Decoder tiling is a memory-management technique; it does not make a terrain texture seamless.

## How resolution was selected

The resolution audit measured how far a texture stretches across its associated meshes. For each measured texture it computed the surface-area-weighted 95th percentile of the major UV-to-mesh Jacobian span, then compared that with a frozen reference budget:

```text
relative_stretch = measured_p95_span / reference_span
requested_resolution = 1024 * relative_stretch
target = smallest available tier covering that request, capped at 2048
```

The reference came from the median of measured texture spans in the initial guild-hall audit. The global audit required reliable coverage of at least 95%, resolved mesh uses and complete source discovery for automatic tier selection. Unresolved entries defaulted to 1024 and retained their uncertainty flags. Rectangular additions used an appropriate longest-side tier while preserving aspect ratio and avoiding shrinkage of eligible larger sources.

These are **mesh-local measurements**, not calibrated meters or a camera-dependent screen-space budget. A large score is a useful review signal, not proof that an image needs a particular resolution. Degenerate UVs, incomplete bindings and shared uses can distort rankings. The 2048 cap was a production policy, not a universal engine limit; stock assets with a 4096 side were found. A higher-resolution experiment still needs format, material and in-game validation.

## Material classification matters more than filenames

A texture that looks like ordinary grayscale or colored artwork may actually control blending, tinting, ambient occlusion or another shader operation. Conversely, some files named `tint` and some secondary texture slots really do contain visible artwork. Trace the material template, texture slot, sampled channels, UV set and blend operation before deciding what can be generated.

The production selection keeps compositor inputs, normal/bump maps, specular and other data maps, effects, UI, unknown roles and unsupported formats stock. Mixed color/control uses are held. Missing protection metadata must stop selection rather than admitting more images.

One crash investigation traced enlarged appearance-compositor inputs to a fixed **512 × 512 DXT1 compositor canvas**. Missing female and LOD references had allowed unsafe inputs through the earlier selection. The fix protected every referenced input with its original bytes. This is a restriction on those specific inputs, not a rule that all armor or character textures must stay 512 pixels. Resizing an already modified control map back to its original dimensions does not restore its original data.

Shared archives also explain why 1.0 has no content-category installer: one archive can contain races, weapons, armor and creatures. Optional categories require deliberately rebuilt variants that preserve all unrelated members.

## DDS and archive compatibility

Keep source identity and texture semantics throughout the conversion:

- Preserve the original DXT1, DXT3 or DXT5 format; do not substitute BC7 without separate compatibility evidence.
- Restore the source alpha, deterministically resized to the target dimensions, onto generated RGB. Alpha can encode material behavior as well as opacity.
- Generate color and alpha mipmaps separately. The production encoder used BOX filtering with `texconv -sepalpha`; earlier premultiplied filtering darkened low-alpha colors and produced black-looking armor at distance.
- Preserve the source mip-tail convention. A single-level source stays single-level. Enlarging a mipmapped texture may add larger levels; preserving the convention does not mean preserving the numerical mip count.
- Validate DDS dimensions, payload length, mip layout, header convention and exporter trailer. Unsupported headers or formats stay stock.
- Build each replacement VPK from a verified stock archive. Replace only the selected entries and preserve untouched packed blocks, names, case and index conventions. Compare hashes and bytes; compressed sizes do not establish equality.

DXT encoding is lossy. PNG masters and VPK/ZIP compression are lossless. Preserve the lossless images and generate later revisions from those or a verified source, rather than repeatedly decoding and recompressing the shipped DDS files.

## Corrections used in 1.0

### Repeating 2 × 2 bias

A repeating pixel-phase pattern was visible in many generated surfaces. The correction estimated a per-channel offset for each of the four `(y mod 2, x mod 2)` phases using a sigma-1 Gaussian high-pass, centered the four offsets to zero mean, and subtracted the corresponding offset from the original RGB. It used float64 calculations, reflect boundaries, rounding to nearest with `numpy.rint`, and clamping to 0–255. Alpha was copied unchanged.

The Gaussian filter was used to estimate the bias; the output image itself was not blurred. This removes the measured global alternating component, not all noise or every possible texture artifact. Processing receipts prevent applying it twice.

### Terrain repetition

For 169 verified repeating terrain bases, a separate pass balanced opposing edges and then restored broad source color. Admission required material and UV evidence of repetition; uncertain cases and character atlases were excluded. Edge continuity does not remove recognizable repeated objects or large motifs. A general-purpose seamless-texture treatment is inappropriate for a character sheet with separate UV islands.

### Character seams and layered faces

Adjacent polygons may sample distant parts of one image, different images or different UV sets. Blurring nearby pixels on the flat sheet therefore cannot reliably repair a visible eyelid, ear or neck seam.

The accepted human/Erudite method used paired mesh boundaries to measure generated-minus-stock color drift, then applied bounded smooth RGB offsets near supported joins. It retained generated fine detail and faded the adjustment away from the boundary. Stock supplied a reference for the intended difference across a join, rather than replacing whole islands. Constant island-wide offsets left some seams; broad stock restoration removed too much generated detail. Hard-normal boundaries, ambiguous shared pixels and unproven joins remained outside the repair.

Ratonga required a different approach: displaced eye-socket detail appeared in the base **and fur-pattern layers**. Repairing only the base left the combined face wrong. Local source restoration was applied to bounded socket regions in the base and nine affected patterns, using each layer's actual UV routing and feathering into the generated pixels. The separate eye/teeth sheet stayed current. For the traced color stage, composition was:

```text
base.rgb * (base.a + (1 - base.a) * tint.rgb) * pattern2.rgb * pattern3.rgb
```

Absent patterns contribute 1. This is one traced material family, not a formula for every EQ2 shader. Inspect the composed model, isolated layers, source appearance and mip behavior; a good flat texture or a numerical seam score is not sufficient validation.

### Final color finish

The 1.0 exporter applied contrast followed by saturation exactly once, to RGB only:

```python
rgb = clip(rint(127.5 + 1.15 * (rgb_float64 - 127.5)), 0, 255).astype(uint8)
rgb = ImageEnhance.Color(Image.fromarray(rgb)).enhance(1.15)
```

This uses a fixed contrast midpoint and Pillow's color enhancement; it is not the same as every application's “15% contrast” control. The original alpha is preserved. The finish and selected repair overrides were applied during packing, leaving original generation masters separate. Do not use already finished preview images as export inputs and apply the finish again.

## Validation and storage workflow

Freeze input/model/code hashes, texture roles, target dimensions and selected revisions before work begins. Use resumable per-image and per-archive checkpoints, verify their input bindings on resume, and serialize publication of the completed pack. A changed recipe needs a new plan; it should not silently reuse stale caches.

Keep one current lossless PNG collection, distinct repair candidates and small provenance/review records. Stage archives as needed rather than keeping permanent duplicate DDS/VPK datasets. Preserve originals while testing candidate revisions. If stock archives are removed to save space, future source-based generation or a full rebuild requires verified stock again; a changed game patch must not be silently substituted for the old source hashes.

Validate protected bytes, source alpha, DDS/mips, every complete VPK and the final ZIP. Review on original meshes with matching UVs/cameras, then test in game. The [gallery provenance](gallery-provenance.json) identifies the source pack, geometry, camera and rendering assumptions for the public comparisons. Preview lighting, animation, customization and shader omissions limit what those images prove.

## Building and contributing to the installer

The installer is a separate Python/Tk application. It does not run the AI model. Use Windows with 64-bit Python 3.12 and Tk support, from a clean checkout:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r src\texture_swapper\requirements-build.txt
.\.venv\Scripts\python.exe -m unittest discover -s tests
.\.venv\Scripts\python.exe build_installer.py --manifest release-manifest-1.3.0.json --version 1.3.0
```

The versioned `release-manifest-1.3.0.json` in this checkout describes direct VPK delivery. `--version` is the **application build version**, independent of the texture-data version (1.2.0 for this release). The builder stages the exact manifest, its SHA256, the app version and texture-data version into the bundle without modifying source manifests. Use `--output-root` to choose a separate build/output directory. The generic `release-manifest.json` remains a legacy test fixture; use the versioned manifest for this build.

The portable build is written beneath `dist/`; build records and caches go beneath `build/`. Rebuilding is refused when the existing output contains portable user data. These commands build the application, not a new texture pack. Dependency pins are in [requirements-build.txt](../src/texture_swapper/requirements-build.txt); the shipped bundle includes a build manifest and third-party notices. Rebuilt executables are not promised to have identical bytes.

Relevant source:

- [`texturequest_app.py`](../src/texturequest_app.py): consumer interface and explicit install flow.
- [`texturequest_direct.py`](../src/texturequest_direct.py): direct VPK downloads, one ready cache and migration from legacy prepared files.
- [`texturequest_release.py`](../src/texturequest_release.py): manifest validation, resumed downloads, part/ZIP integrity and preparation.
- [`texturequest_updates.py`](../src/texturequest_updates.py): verified base/update overlay, unchanged archive reuse, and resumable preparation of the full effective inventory.
- [`texturequest_version_check.py`](../src/texturequest_version_check.py): independent app/texture notices, saved version floors and official upgrade links.
- [`texture_swapper/core.py`](../src/texture_swapper/core.py): archive validation, installation preflight, copying and receipts.
- [`tests/`](../tests/): integrity, recovery and UI-state fixtures.

Changes should retain explicit installation, closed-game checks, path containment, interrupted-download recovery, checksum verification and safe handling of changed files. Test against temporary fixtures before a real game installation. Texture reports are most useful with a precise item/NPC/zone or appearance, a screenshot, pack version and verification result.

For a future release, publish new immutable assets rather than replacing the bytes behind existing links. An update containing only changed archives still needs complete archives with every unrelated replacement preserved. Model terms and original game-asset ownership are separate from the installer source; consult the upstream model terms and the project's [ownership notice](../README.md#credits-and-ownership).

### App and texture update versions

Manifest v3 publishes each complete VPK as an individual immutable GitHub
release asset. Its `texture_version` describes the artwork; its top-level
`version` describes the publication. App 1.3 retains texture contents 1.2.0.
Every archive row gives its name, byte count, SHA256, native entry count and
official download URL. Size rejects an obvious mismatch quickly; SHA256
determines whether equal-size files have the same bytes. Unchanged archives can
keep their earlier direct-asset URL in future releases.

Ready files live once in `data/downloads`. Preparation verifies existing files,
resumes interrupted downloads and moves matching legacy prepared files into that
cache. Only recognized obsolete containers are removed after the entire ready
inventory is verified. Changed or unrecognized files are preserved. Installation
copies from the same cache; a future sideloader can read it without another full
dataset. There is no texture ZIP extraction step in direct delivery.

The v1/v2 contracts below document older releases and retained test fixtures.

The application build version, the GitHub publication tag, the manifest envelope version and the texture package version are separate values. `texturequest_build_pin.py` embeds `APP_VERSION`, `TEXTURE_VERSION` and `MANIFEST_SHA256`; frozen UI/runtime checks report those actual values. `src/texturequest_version.py` is only the source default. The texture-data version is `update.version` for manifest v2, or the base version for v1. Texture byte identity hashes the sorted complete archive names, sizes and SHA256 values, excluding display metadata and envelope versions.

The builder emits a small `dist/texturequest-version.json` for publication alongside the installer ZIP and exact `release-manifest.json`. Its `texturequest-version-v1` contract has exactly these fields:

```json
{
  "schema": "texturequest-version-v1",
  "app_version": "1.2.1",
  "texture_version": "1.2.0",
  "manifest_version": "1.2.1",
  "manifest_sha256": "<SHA256 of the exact release-manifest.json asset>",
  "installer_name": "TextureQuest-Installer-1.2.1-windows-x64.zip"
}
```

The GitHub release API supplies the version asset's size and SHA256. Its verified contents must bind the published manifest and installer asset; every URL belongs to `Maergoth/TextureQuest`. **For an app-only 1.2.1 publication, retain legacy compatibility by changing only the manifest's top-level `version` to `1.2.1`. Keep `update.version` at `1.2.0` and every archive identity, base record, update record and package URL unchanged.** Publish this small wrapper manifest, the version metadata and the new installer under `v1.2.1`; no texture package is reuploaded. The already shipped checker requires the envelope version to equal the GitHub tag. An old byte-exact 1.2.0 manifest under `v1.2.1` is supported by the new explicit metadata checker, but would break those old apps and is not the recommended publication shape. A texture-only release may retain the same application version. Already published releases without the metadata asset remain compatible: the actual app version comes from a unique, checksum-bearing official Windows installer asset, and the texture version comes from the validated manifest.

Startup and manual checks fetch one stable published release record and report the two channels independently. A missing/invalid installer does not prevent a compatible pack check; a manifest download/schema error does not hide an available app update. Invalid shared release/version metadata fails both channels. Drafts, prereleases, foreign URLs, older texture data and changed bytes under an unchanged texture version are refused. Network and GitHub limit errors remain unavailable states, never an "up to date" claim. A failed texture check retains the saved release and downloads. A locally saved receipt establishes the selected version floor on restart; it is not a successful live check and cannot enable installation by itself.

In that historical version, the app upgrade action only opened the verified release page. Current Get app update downloads and verifies the official installer, replaces the application and restarts it, preserving data/downloads. It requires active hooks to be uninstalled and EQ2 closed before replacement. Texture installation remains a separate explicit action.

### The 1.1 4K update

The 40-map batch preserves 35 saved 1K/2K generations as inputs and uses five verified retained original 1K maps for the previously excluded Leviathan body family. Those five are visible RGB artwork whose duplicate color/alpha shader binding was resolved separately; source alpha remains tint data. The Qwen recipe retains 20 steps, CFG1 and seed0. Output has a 4096-pixel longest edge, preserving aspect ratio; conditioning still runs at approximately one megapixel. Original saved masters are not replaced.

Only nine complete PAKs change. Their verified 1.0 versions provide the baseline; every unselected compressed entry is preserved and compared byte-for-byte. Selected DDS entries use their proven original format, header/trailer convention, source-derived alpha and mip tail, with all generated mip levels checked. The color finish above is applied once to the new raw PNGs. Existing finished DDS entries are copied without reapplying it, preserving the twenty earlier character/creature repairs. The effective manifest retains all310 archives and increases the replacement count from23,277 to23,282.

Versioned manifest v2 embeds the exact v1 base and adds the changed-archive package plus the full effective archive inventory. Cached or installed files are reused only after matching their hashes. Preparation does not write to the game. The installer verifies its bundled manifest, then checks GitHub's latest published release on startup and before preparing a pack. It verifies the downloaded manifest against the release asset's SHA-256 and size and requires same-repository package URLs. Legacy release envelopes must match their publication tag; explicit version metadata provides separate application and texture pins as described above. Compatible v1/v2 textures do not require new executable code. Unsupported formats require a newer installer. An app-only release may reference unchanged texture packages; an identical complete prepared pack is reused without making another archive copy.
