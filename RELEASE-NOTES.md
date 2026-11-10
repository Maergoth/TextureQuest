# TextureQuest 1.13.18 — Private comparison candidate

Press **Home**, select **TextureQuest**, then open **Advanced**. The existing
comparison switch is now **Experimental improvements**. It starts off each launch;
unchecked keeps the current 1.13.16 lighting path. Existing graphics settings remain.

The candidate blends valid same-owner/same-slot emissive-shadow endpoints over
150 ms, pairing each
static map with its actual moving-caster snapshot and matching source metadata.
Current animated light colour is retained. Missing, partial or incompatible history
uses valid current shadowing; reassignment and settings/epoch/zone changes abort
the fade. Six logical transitions share lazy history pools capped at 84 MiB.
Ranking or shadow-slot turnover remains current-only. A frozen moving silhouette
can briefly trail or double during the 150 ms fade; this is not motion reprojection.
The 1.13.17 source observations and independent visibility remain. Map resolution,
filtering and normal update/light budgets are preserved.

Comparison logs include transition_lights; summaries and the UI show the mean
number of transitioning lights.

Use **Record comparison performance (CSV)** to compare the same scene and camera.
Allow at least three seconds and 180 frames after switching, and let caches settle.
Logs and summaries are under `EQ2Graphics-captures/lighting-comparisons`. GPU lighting
timings exclude source observations and shadow-map rendering. No hardware FPS or
universal visual improvement is claimed by this private candidate.

Use the normal updater with all EQ2 clients and TextureQuest closed. Keep portable
data. Texture data stays 1.3.0; unchanged preparation is reused without DDS/map
verification or texture scanning. This is not a public release.

See [the changelog](CHANGELOG.md) and [comparison handoff](docs/GRAPHICS-1.13.18-HANDOFF.md).

---

# TextureQuest 1.13.15

The Develop graphics work is now packaged for the public installer with simpler
quality presets and safer new-install defaults. Texture data remains 1.3.0.

**Press Home to show or hide settings**, then choose **TextureQuest**.

- Ambient occlusion starts on. Point/emissive lights, sun lighting/softening,
  smooth interface text, frame generation and neural rendering start off.
- Upscaling and Reflex driver mode are removed from the active runtime and controls.
- Shadow quality and Light quality offer Low / Med / Max. Advanced retains manual
  budgets and tuning; existing users' saved settings are kept.
- Includes stable particle groups, short-lived light retention, supported model-normal
  replay, particle character shadows up to 128 lights and bounded self-occlusion reduction.
- Keeps the later F10/menu compatibility fix and unchanged texture preparation reuse.

Update through Get app update with EQ2 closed. Keep your portable data folder.
No texture dataset download is required when the existing texture preparation is unchanged.

Sun-direction movement flicker remains unresolved. Particle coverage, unsupported
model shaders and sources the game never submits retain their documented limits.
Controlled checks do not establish universal in-game appearance or FPS improvements.

See [the changelog](CHANGELOG.md) and [graphics guide](docs/GRAPHICS_FEATURES.md).
