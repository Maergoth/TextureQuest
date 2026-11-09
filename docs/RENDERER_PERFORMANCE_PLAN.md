# World renderer performance plan

Plan for a performance pass over `graphics-renderer/`. The game is CPU-bound on its render thread, so CPU work on that thread ranks first; GPU savings make room for more lights and features.

## Baseline

Capture `EQ2Graphics-captures/20261102-043207`, 3840x2160, 103 fps, ~1,500 game draws per frame:

| Measure | Value |
|---|---|
| CPU "game draws" (our hooks on the game's draws) | 1-3 ms (overlay, not recorded in captures) |
| CPU "lamp shadow maps" (end-of-frame map drawing) | under 1 ms (overlay, not recorded in captures) |
| Moving-object shadow draws | ~940 per frame (97,093 per second) |
| Lamp shader draws | ~285 per frame (29,046 per second) |
| Casters per lamp map | median 1,418 (41 lamps) |
| Casters per glow light map | median 910, max 1,799 (180 glow lights) |
| Cached objects | 2,114, of which 1,667 unseen for 30 s or more and 818 unseen for over 16 min |
| GPU | occlusion 0.884 ms, sun 0.537 ms, glow 0.318 ms, surface 0.194 ms |

Every item is compared against this capture afterwards.

## 1. Replace the shadow range slider with each light's own reach

`shadowmaps::Range` (the "Shadow range" slider, 5-200, set to 200 in this install) is used as the shadow radius of every light: casters within it go into a light's static and moving maps, it is the cube map far plane, and it sizes the screen rectangle a light's shadows cover. Lights themselves are chosen by distance from the camera and the light limit, not by Range. At 200, every lamp gathers casters far past where it gives light, which is why each lamp map holds ~1,418 objects and a moving object is drawn into all 16 moving maps.

A surface outside a lamp's light gets nothing from that lamp, and anything that shadows a lit surface lies between it and the lamp, so a lamp's reach bounds its shadow casters exactly.

**Per-light radius (no constant):**

- A lamp's radius is `Reach * FADE_EXTENT` (reach × √2, the farthest point of the game's fade cylinder). This sets its caster set and its cube map far plane.
- Static casters are tested by the nearest point of their bounds when `BoundsKnown`, otherwise by origin. This also closes the large-mesh gap noted at `shadow_maps.hpp:1245`.
- Moving casters are tested by origin plus `MESH_RADIUS` (`shadow_maps.hpp:2495`).
- `DrawCubeMap` draws each caster only into the cube faces it can reach, reusing the face test from `DrawMovingMaps`.

Logged lamp reaches across all captures (1,334 entries): about 700 at 1.3-10, about 270 at 12-19, one lamp at 38.81, one at 70.64. No real lamp exceeds 70.64.

**Constants that replace the slider:**

| Use | Today | New | Evidence |
|---|---|---|---|
| Radius of a lamp whose reach cannot be read (`Reach <= 0`) | 200 | 100 | Largest logged lamp reach is 70.64, so ~100 with √2 |
| Glow light radius (`GlowRange`, also where its light fades to zero) | 200 | Per emitter (see below), capped at 200 | 6 large emitters stay at 200; 28 small or dark painted glows drop to 23 or less |
| Probe distance from the camera (`SendProbes`) | 200 | 200 | All 448 objects drawn in the last 30 s are within 100; nothing beyond 100 was probed in 600 frames because those spots were off screen |
| Cache keep distance from the camera (`Evict`) | 800 | 400, and forget an object unseen for 10 minutes if no current light holds it as a caster | Nothing lies beyond 400, so nothing was ever forgotten; 818 objects unseen for over 16 min |
| Held moving-mesh distance from the camera (`HoldMovingMeshes`) | 400 | 100 | Every held mesh with a real position is within 50 |
| Glow emitter distance from the camera (`IsCellSeen`, `FindCut`) | 800 | 400 | Every emitter seen in the last 30 s of its capture was within 200 |

The slider and its `ShadowRange` setting are removed.

**Glow light radius.** The radius is computed once per emitter, from its total brightness: brightest radiance channel × whole area / π, times the glow strength and, for painted glow, the painted light share. It is the distance where that brightness over distance² falls below 1/255, capped at 200. Every cell of an emitter uses its emitter's radius, and the radius is recomputed when a strength setting changes. Glow lights originally took their reach from their own cell's area, and walls in the lava cave faded as the camera approached, because cells split smaller near the camera. A per-cell radius would bring that back, and would also cut light that many faint cells add up to. Across 35 emitters in the captures:

| Emitters | Count | Size | Light invisible at |
|---|---|---|---|
| Painted glow, effectively dark | 9 | 0.5-9 | 0-1 |
| Small painted glow | 19 | 2-8 | 3-23 |
| Medium painted glow | 1 | 102 | ~126 |
| Lava fields and large painted areas | 6 | 450-760 | 1,500-4,700, so 200 with the cap |

**Cache age limit.** Objects that are off screen but within a lamp's light keep casting shadows. That is why an earlier 30-second drop was replaced by distance-based eviction and probes. So the 10-minute limit only forgets objects that no current light holds as a caster; a player standing still keeps the shadows behind them.

**Glow emitter distance.** Glow lights are capped per frame (`MAX_GLOW_LIGHTS`), so this distance does not change cost; 400 relies on bounce light to fill large caves. Lava cells beyond it give no light or bounce at all, so this is the value to raise if a large cave looks dark at its far end. It was widened for the lava cave, where picked cells sat 260-330 from the camera in a cave about 470 across.

**Meshes in world coordinates.** 132 static cached objects in the baseline have an identity transform at the world origin: their vertices are already in world coordinates, so their origin says nothing about where they are. 54 of the 92 held meshes, all of those anchored at 0,0,0, are presumably the same kind. Distance tests by origin are wrong for these meshes, so:

- read the bounds of world-coordinate moving meshes once, the same way static casters' bounds are read, and cull them by bounds in the moving-map reach test;
- use those bounds, not the anchor, in the held-mesh distance test.

**Debug colour:** add one for lit pixels outside a lamp's computed radius, so a radius that is too small shows in game.

## 2. `FindShadowSlot`

`FindShadowSlot` (`shadow_maps.hpp:986`) runs on every lamp draw (~285 per frame) and does a `Lights.find` for all 256 slots before checking `SlotActive`, about 72,000 hash lookups per frame inside "game draws".

After `AssignSlots` each frame, build a flat list of usable slots: active, not glow, built, and holding casters or a used moving map, with each slot's built position and hue. `FindShadowSlot` scans only that list. The result is unchanged.

## 3. Measurement

- **Split `MapCpu`:** separate CPU timers for static and moving map drawing.
- **GPU timers** for shadow map drawing and for lamp shading. Move `GpuTimer` and its start/stop/finish functions somewhere `shadow_maps.hpp` can use them.
- **Counters:**
  - static caster draws per second;
  - lamps with no readable reach, to validate the fallback of 100.
- **Capture files:** write every CPU timer into them, so before/after comparisons are on record.

## 4. Per-draw hook overhead

This is the largest measured CPU cost: 1-3 ms in "game draws". From reading the code:

- **The same draw's state is read repeatedly.** One base-pass draw reads, separately in each handler:
  - the viewport 3 times;
  - the blend state and its description 3 times;
  - the game's constant rows twice, each a `Map`/`Unmap`;
  - the vertex and index buffers 3 times, with the mesh hashed 3 times.

  That is about 25-30 COM calls per draw. Gather this once per draw into a struct and pass it through `HandleDraw`, `RecordBaseDraw`, `Record`, `KeepBaseDraw`, `ReadSunDraw`, `NoteBlendedDraw`, `glow::HandleColourDraw` and `particle_emission::Observe`. Classify blend and depth states when the pipeline is created. The struct never outlives the draw.
- **Locks:** up to 6 `shadows::Lock` acquisitions per draw, plus hash-set lookups. Classify shaders in `capture::OnBindPipeline` into one bit mask per shader and blend state, so the draw path reads bits without a lock. Only `init_pipeline`/`destroy_pipeline` take the lock.
- **Guard:** `Master::Device` (`texturequest_world_master.hpp:14`) runs `compare_exchange_strong` on every event. Load first, and compare-and-swap only while it is empty.
- **Sun-path repeat calls:** when the sun-direction path in `shadows::HandleDraw` (`shadows.hpp:2410-2422`) consumes a draw, it calls `CountCasterDraw` and `OnAnyDraw`. ReShade still runs every other draw callback, so `lightbuffer::OnDraw` (`light_buffer.hpp:3157-3159`) calls both again. Nothing is counted twice, because that draw targets the light buffer, not a cascade map, and `OnAnyDraw` is safe to repeat. Remove the repeat as part of the per-draw struct work.
- **`KeepBaseDraw`** scans every light per moving draw. Build a flat list of remembered lamps once per frame, and check `KeptDraws` before hashing.
- **`NoteBufferWrite`** does two hash-map operations on every written buffer (6,496 tracked). Merge them into one map and track only buffers seen as vertex or index buffers.
- **`shadowmaps::OnDestroyResource`** scans up to 4,096 classified meshes on every destroyed resource, even with every feature off. Use a reverse index from buffer to mesh.
- **Capture-only hooks:** register them only while a capture is running.

## 5. Instanced shadow drawing (conditional)

Do this only if item 3 still shows significant shadow map CPU after item 1.

- **How:** draw each moving object once for every light and cube face it reaches. The (light, face) pairs come from a per-frame buffer, and the cube face is written from the vertex shader. Draws drop to the number of unique objects.
- **Why it is feasible:**
  - The caster shaders read only `POSITION`.
  - The game never uses instancing (all 4,059 captured draws are single-instance).
  - Writing the render-target array index from the vertex shader is supported on roughly GTX 900, Radeon HD 7000, Intel 6th gen and newer.
- **Older DX11 GPUs** use a pass-through geometry shader instead. Log which path is in use at startup.
- **Skip the step in between:** this makes per-draw constant batching unnecessary.

## 6. GPU

- **Ambient occlusion:** `ps_occlusion` computes half the texels each frame in a checkerboard and keeps the other half's previous values, a half-rate approach matching MXAO. It does this by discarding, but GPUs shade in 2x2 blocks and every block keeps 2 texels, so nearly the full cost is still paid. Compute only the active texels instead, for example with a compute shader writing them into the existing target, so the other half keeps its previous values exactly as now. The image is the same.
- **Glow readback buffers:** `ReadTriangles` creates a default and a staging buffer of up to 4.7 MB each, plus a zeroed vector, per request, up to 16 per frame. Use a buffer pool. This is a zone-in hitch, not a frame-rate cost.
- **Lamp shadow filter:** decide after its GPU timer. `SoftShadow` takes 16 blocker samples and 16 filter samples per lit pixel, and twice that with a moving map. Planned options:
  - skip the moving-map lookup on cube faces where no moving object was drawn (no visual change);
  - fewer samples for distant or small lamps. Check in game for noise in distant soft edges, and against the distance-based softening fix in 0b718e9.

  A 4-sample early exit is held back, because it narrows the outer fringe of soft shadows.
- **Occlusion upscale computed twice at 4K** (`ps_apply` and `ps_scale`): only happens when the sun fix builds its scaled image, which was not running in the baseline. Revisit if the timers show it.

## 7. Final review

Review the whole implementation for correctness, regressions and consistency with the surrounding code. Then take a new capture and compare it against the baseline: game draws and lamp shadow map CPU, moving draws per frame, casters per lamp and per glow light, cached object count, and GPU timers.

## Not planned

- **Cap on moving-lamp map redraws:** moved lamps redraw their full map every frame with no limit, but the baseline shows only 9 maps drawn per second.
- **Handing held meshes to the static cache:** held meshes keep being redrawn every frame.
- **Original-mode costs in the TextureQuest add-on:** Original mode is only for brief comparisons.
- **Caching the surface pass's render target view:** it only runs while surface saturation is above 0, which this build forces off.

## Later: emissive lights sampled over several frames

This is a feature, not part of this pass. Keep each emitter's cells and their cached shadow maps. Each frame, a pixel evaluates a rotating subset of the cells (for example 16 of 64) scaled to stand for all of them, and jitters each cell's sample point within the cell. Results are blended over a few frames, reprojected with depth and the camera matrices. This should allow about four times as many glow lights at the same per-pixel cost, and a true area-light look up close.

Costs: a history buffer, reprojection, guarding against ghosting on moving objects and newly revealed areas, and testing alongside DLSS frame generation. Fully random points every frame do not fit, because each new point would need a new cube shadow map.
