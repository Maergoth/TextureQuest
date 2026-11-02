# TextureQuest 1.13.6 graphics handoff

These are the current fix attempts for Tenri/Sichii to pull and review.
Publication precedes local installation; there is no 1.13.6 local-game visual
or performance result. `graphics-runtime/RUNTIME.json` selects the exact release
images. Each native component's RELEASE-SOURCE/RELEASE-NATIVE-SOURCE metadata
records its source and build inputs. Dependency licenses remain intact.

## Changes and limits

| Area | Attempt | What remains to establish |
| --- | --- | --- |
| Loose DDS reads | Reuse full-SHA-verified immutable bytes in a per-reader 16-entry/64 MiB LRU; validate row geometry on hits | Actual EQ2 load-time/FPS effect; active thread pins can temporarily retain evicted buffers beyond cache-owned bounds |
| Surface saturation | Restore the optional world-surface color path and controls | Appearance across dark, clipped, sky, specular, fog and emissive scenes; it changes color, not light energy |
| Material normals | Route supported material/normal inputs with conservative fallbacks | Partial coverage; unknown shader/material paths and full-world normal correctness are unproved |
| Emissive budgets | Expose emissive limits 1–256 and particle limits 1–32, with absent defaults 64/4, within the shared Shadowed lights pool; native lamps take priority | Higher limits cost CPU/GPU time and do not guarantee emitter coverage |
| Particle packets | Keep a finite remote or oversized primitive from invalidating the whole packet and suppressing nearby light | Flicker, offscreen/culled-source behavior, and the cause of every purple square/box remain unproved |
| Sun borders | Exclude invalid gather lanes from shadow contribution, divide by the original tap count and treat unavailable samples as unshadowed | Controlled border/sentinel success does not prove the actual outdoor box artifact's cause or full in-world removal |
| Sun movement | Retain bounded source/history handling and diagnostics | Movement/shadow-direction flicker remains unresolved. Histories are diagnostic; no general cure is claimed |
| Softness controls | Show both shadow-softness controls on a 1–10 UI scale; preserve existing stored appearance until adjusted | Opening the UI does not migrate or clamp saved values; Sun Off remains separately controlled |
| AIO startup | Both absent FrameGeneration defaults are off; explicit saved settings win | The compile/default audit adds no new frame-generation quality or real-game proof |

The DDS fixture passed 185 native CPU/filesystem checks with 47 instrumented
Reader SHA calls, and the main add-on compiled. Physical symlink creation was
denied by OS1314 and skipped; a separate injected reparse-attribute case passed.
Previously admitted session bytes survive later disk mutation/deletion until
eviction or a new reader. No file handles, disk cache or worker is retained.

Controlled renderer fixtures use captured native shaders with synthetic inputs;
they do not establish all native callbacks or combined in-world behavior.
Particle alpha and radial behavior were already present in 1.13.5; the 015
cases reconfirm those retained components. Earlier results carry forward only
for unchanged tested components, with separate coverage for changed paths.
Reflex remains partial device-local integration with no measured latency benefit.

## Follow-up review

Compare stationary/moving views of the same spell or fire; the same material
with/without its normal path; dark, bright and specular surfaces with saturation
toggled; and a Sun border through camera and Sun movement. Retain the feature
states and a minimal native diagnostic capture for a reproducible defect.
The earlier shadow-alignment report also reproduced without TextureQuest
features, so its underlying cause remains separate and unproved.

Keep local captures, stock game binaries and stock textures out of the source
repository. State the scene, camera motion and whether the defect also occurs
with the relevant feature off.

Texture data stays at 1.3.0. Existing archives/URLs and native game INIs are
preserved. Native bloom remains the player's choice. Press Home for controls;
keep native lighting/GPU shadows available for features that consume them.
