# Lighting comparison candidate 1.13.17

This local build keeps the current 1.13.16 renderer and adds an opt-in candidate.
Saved graphics preferences and texture data 1.3.0 remain unchanged. It is not a
public release or a claim that the reported scene is fixed on hardware.

## Comparing the paths

1. Press **Home**, select **TextureQuest**, then open **Advanced**.
2. Enable the lights you want to compare and **Record comparison performance (CSV)**.
3. Leave **Test lighting improvements** unchecked to collect the current path.
   Keep the same scene and camera, and let the light caches settle.
4. Check **Test lighting improvements** to collect the candidate. The switch
   commits at a frame boundary and rebuilds source caches. Both columns retain
   their samples. Recording excludes a minimum of three seconds and 180 frames
   after each switch from its summaries.
5. Repeat the same camera movement for both paths. Stop recording to save a
   summary alongside the detailed CSV. **Copy log path** locates the files.

The experiment resets to unchecked on each game launch. Recording also starts
off. Changing other hook settings or render dimensions clears both summaries;
**Clear comparison results** starts a new warm-up without changing the mode.
The table compares sequential runs; it does not render both versions at once.

Logs are under the game executable's directory:
`EQ2Graphics-captures/lighting-comparisons/lighting-<UTC>-<PID>.csv`, with a
matching `.summary.csv` on stop or normal device release. Detailed rows include
mode, epoch, frame, settings identity, light count, dimensions, warm-up, fallback
and validity. `-1` denotes an inapplicable timing. Summary mean and p95 use the
latest 600 valid samples per mode; the total GPU sample count is also retained.

**GPU lighting** covers the candidate list build, reduced lighting and full
resolution reconstruction. It excludes source observations and shadow-map
rendering. **Game Present** intervals include the game, those other passes,
caps and logging overhead; they are not display FPS or generated-frame FPS.
Timestamp reads never flush or wait for the GPU. CSV writes are buffered and
flushed once per second; their CPU duration is a separate event. Keep caps,
frame generation, game options and background workloads unchanged during A/B.

## Candidate scope

- The verified particle vertex profiles expose source geometry before their
  camera-distance size clamp. The private observer uses that geometry while
  retaining the native material shader, animated UVs and alpha behavior. Other
  particle profiles retain the current observer. No global RGB smoothing is
  introduced; intentional fire variation remains.
- One verified painted-object pair (`d9379a14dac0f2ed` / `7a8c2bd9056f65be`)
  replays complete material triangles into a private atlas with native texture,
  sampler and constant bindings. Stable primitive anchors place the measured
  flux into world cells. Candidate coordinate keys avoid a demonstrated signed
  coordinate collision that could multiply energy through the hierarchy.
- Candidate direct lighting uses each light's own shadow visibility. It removes
  aggregate visibility borrowing and independent weak-light tail truncation.
  Conservative receiver-tile bounds reduce shadow evaluations; small light sets
  use the exact independent path. Map resolution and filter patterns remain.

Supported object observations neutralize source fog only in a private constant
copy. Alpha discard remains native, and additive source RGB is not multiplied by
alpha again. Unsupported or changing material modes revert to the current path.
Resources are private and fenced; native render state and constants are restored.
If a source changes while its snapshot is pending, that exact historical snapshot
remains usable and requests another eligible observation after a bounded 15-frame
interval, rather than the ordinary 180-frame cooldown.

The receiver-tile allowance is at most `1/2048` per RGB channel of raw direct
lighting, after uploaded intensity/painted/area scaling and shadow strength.
Bounce is unchanged. This is not a bound on later game exposure, bloom or target
quantization. An additional exact bounce-culling mask was measured and rejected
because its construction/lookup cost made the software fixtures slower.

## Controlled performance result

The final software Direct3D fixture includes list construction, lighting and
reconstruction. At 512 distributed lights, the candidate took about 0.65 times
the corrected full-loop time and 0.88 times the legacy time. At 512 overlapping
lights it took about 1.03 times the corrected full-loop time and 4.36 times the
legacy time. The legacy path avoided work by dropping or sharing visibility in
that case. These are synthetic CPU-rendered workloads, not GPU speedup estimates.
The candidate therefore stays opt-in, and actual hardware comparisons are needed.

## Limits and research

The object observer covers one exact shader pair and supported static resource
types, not every emissive material. It uses 28 fixed material samples per triangle;
integration of arbitrary fine texture detail is approximate. Animated UVs and material colour remain live,
but genuinely rotating objects with unchanged mesh buffers can retain earlier
world anchors: camera-facing rotation cannot safely be treated as physical
movement. Sources the game never submits cannot be discovered, and existing
off-screen retention is finite. Unsupported source paths still have their
original camera-visibility limitations.

Independent visibility can cost more than the legacy shader when that shader
obtained speed by discarding or borrowing shadow visibility. The comparison
switch exists to measure the tradeoff rather than claim a universal speedup.
The inherited aggregate-dependent filter sample count remains; changing shadow
sampling was expressly held out of this implementation. Known sun-direction
and unrelated AO issues are not claimed fixed here.

Tensor Core feasibility was researched. No CUDA, model, D3D12 bridge or Tensor
Core runtime is installed: conversion/interop costs and precision need a measured
prototype first. A paired shadow-sampling research fixture crashed on an empty
measurement batch; the harness now rejects empty cohorts, and the sampling
candidate failed quality checks. It is not part of the hook.

The strict build and final focused validation receipts are pinned in
`graphics-renderer/RELEASE-SOURCE.json`. These use CPU and software Direct3D;
actual EQ2 appearance and physical-GPU performance remain live comparisons.
