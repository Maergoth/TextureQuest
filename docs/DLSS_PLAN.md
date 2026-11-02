# DLSS neural rendering and frame generation plan

Plan for improving the DLSS stack (`graphics-dlss/`, driven by `graphics-addon/aio_controller.hpp`) together with the world renderer (`graphics-renderer/`).

The main feature is DLSS neural rendering (`nvngx_dlssnr`, "NR"), which adds detail to the image. The other features are DLSS-G frame generation (`nvngx_dlssg`, multi-frame on 50-series) and DLSS super resolution or DLAA (`nvngx_dlss`). The game is CPU-bound on its render thread, so the GPU has room for NR. Every GPU millisecond saved in the world renderer (see `RENDERER_PERFORMANCE_PLAN.md`) is room for NR.

## How it works today

- **Capture:** at the end of ReShade's effects (`OnReshadeFinishEffects` → `TQCaptureAfterEffects`), the backbuffer is copied into one of 3 textures shared with the add-on's own D3D12 device. A shared fence then signals a worker thread. The render thread does no waits or readbacks.
- **Motion:** NVIDIA Optical Flow only, on a grayscale copy downsampled to 180p by default, converted to full-resolution vectors. There are no external hints.
- **Depth:** on the default path, NR, SR and FG receive `g_fallback_depth`, a texture cleared once to the far plane (`nr-standalone.cpp:6511`).
- **NR inputs:** colour, `DLSSNR.Depth` and `DLSSNR.MVec` (`nr-standalone.cpp:5375-5378`). So NR gets the constant depth and the 180p optical-flow motion.
- **Frame generation:** real DLSS-G. `DLSSG.HUDLess` is the same image as the backbuffer (`:5470`), and the camera matrices are identity.
- **UI:** EQ2's HUD is in the captured image, so NR processes it and frame generation interpolates it. Only ReShade's own menus are drawn on a separate layer and composited last (`texturequest_aio_overlay.hpp`).
- **Presentation:** a separate D3D12 composition swapchain over the game window, paced by a timer at the display refresh.
- **Reflex:** `NvAPI_D3D_SetSleepMode` only, with no sleep calls or latency markers.

## 1. Measurement

There is no capture with DLSS on yet, so start by recording one:

- NR, SR and FG GPU times from the existing performance telemetry;
- real and generated frames per second, and how many captured frames are dropped;
- the add-on's CPU time on the render thread.

Use the same scene as the renderer baseline (`EQ2Graphics-captures/20261102-043207`), with NR on. Each item below is compared against this capture.

## 2. Real depth

Copy the game's depth into each capture slot next to the colour, and signal it with the same capture fence, with no extra wait or flush. Pass it to NR, SR and FG in place of `g_fallback_depth`. NR can then tell surfaces and edges apart, and DLSS-G can order what is in front at disocclusions.

To check: the depth has to belong to the same frame as the colour and match its size. Depth must also be the world's, not the UI's.

## 3. Exact motion vectors from the world renderer

The world renderer already reads the camera's world-to-clip transform each frame (`light_buffer.hpp` `ReadWorldToClip`) and the camera offset (`shadow_maps.hpp`, row c16).

- **World renderer:** export the current and previous world-to-clip, keyed by frame sequence, through a small ABI next to the existing `TextureQuestWorldGraphicsABI` exports. The game draws relative to the camera, so both transforms must be in one absolute space.
- **DLSS add-on:**
  - a compute pass reprojects depth into exact camera motion vectors at full resolution;
  - where optical flow disagrees beyond a threshold (moving characters, particles), it uses the optical-flow vector;
  - it fills DLSS-G's `ClipToPrevClip` and `PrevClipToClip` instead of identity;
  - optionally, it feeds the reprojected vectors to optical flow as external hints.

Detail added by NR has to move exactly with the surface it belongs to. Full-resolution exact camera motion, in place of 180p flow, should be the largest stability gain for NR and frame generation. Optical flow stays as the fallback and cross-check, so a wrong transform cannot corrupt the whole frame. Optical flow may then run at a cheaper setting.

## 4. Capture before the UI

Keep NR and frame generation on the 3D world only, so the HUD and text are neither enhanced nor interpolated:

- **Cheap:** the world renderer marks the boundary between world and UI drawing. The world image there is captured for NR, and passed to DLSS-G as `HUDLess`. Costs one 4K copy.
- **Thorough:** draw the UI on its own transparent layer and composite it last over real and generated frames, the way ReShade's menus already are.

To check: where ReShade's effects currently run in this setup. ReShade can be set to apply effects on a render target change, which falls between world and UI rendering. If capture already happens there, the UI is missing from the captured image and needs compositing back on.

## 5. Frame pacing

Real frames are admitted at most every `Presented / refresh` (`AdmitNewestFrameForFgPair`), and output is spaced at the display refresh. At ~103 game fps:

- at 144 Hz with 2x, about 30 real frames per second are captured and then dropped;
- at 240 Hz, the output rhythm goes short-short-long.

Changes:

- choose the multiplier from refresh ÷ game fps, minimum 2;
- pace output from the game's present cadence (`g_source_frame_avg_us`);
- skip capture copies for frames that will not be admitted.

Dropped frames also break optical flow's temporal hints (`nvof-motion-provider.cpp:968`), so this improves motion quality too.

## 6. Fixes inside the DLSS add-on

- **Stall in the optical-flow depth option:** with `NvidiaOpticalFlowDepth` on, `CopyLegacyGeometryToD3D12` makes the game wait on the previous frame's NGX work and flushes, the stall the default path avoids. It also renders the whole 4-target Feed technique to get depth. Replace it with per-slot depth copies without a wait (item 2).
- **Unused copy:** `CopyResource(original_snapshot, packed_color)` runs every real frame, but only feeds a comparison view that cannot be turned on in this build. The present shader also reads `Original` and `Post` for every output pixel. Drop both.
- **Render-thread trims:**
  - the overlay `Flush` and 4K clear every frame, even with no ReShade window open;
  - `GetModuleHandleW` twice per Present for Streamline detection;
  - window geometry queries every Present;
  - `AioController::Reconcile` twice per frame;
  - performance telemetry on by default.
- **SR feature always created:** it is created even when SR is off, and a creation failure stops the whole pipeline. Create it only when SR or DLAA is on.

## 7. Reflex

Call `NvAPI_D3D_Sleep` from the game's Present while frame generation is on, so low-latency mode does something. Low priority: in a CPU-bound game the render queue is mostly empty already.

## 8. Final review

Review the whole implementation for correctness, regressions and consistency with the surrounding code, on both the world renderer and the DLSS add-on. Then take a new capture with NR and frame generation on and compare it against item 1: NR stability while turning the camera, UI legibility with frame generation, real and generated frame rates, and render-thread CPU time.
