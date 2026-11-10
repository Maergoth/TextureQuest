# Idle hook overhead in 1.13.23

A textures-only installation still loads the ReShade provider and the
TextureQuest texture add-on. It does not load the optional EQ2Graphics renderer
or NVIDIA/AIO components. Turning their controls off in-game is different from
excluding those components in the launcher and restarting.

## Work removed

- Hidden TextureQuest FPS no longer samples the clock or AIO output counters.
  The provider's optional overlay-demand API lets the existing GUI early return
  run when that overlay is hidden and no other GUI element needs rendering.
  Home, third-party overlays, messages, previews and ReShade statistics retain
  their original admission conditions. Registrations stay stable while toggling.
- Once owned effects are confirmed disabled or absent, reconciliation skips
  repeated SDK lookups. No technique handles are retained across callbacks.
  Reload, requests, master changes and technique events invalidate the copied
  facts. Active effects still check current handles and depth readiness.
- Capture-only callbacks reject idle work before device/native-context queries.
  Shader/resource lifecycle metadata remains available for a later capture.
- When the optional renderer is loaded, inactive sampler callbacks reject work
  before device queries and timing. Idle lighting diagnostics avoid frame clocks,
  context queries and settings hashing; start, stop and reset still execute.

## Quality and evidence

No texture bytes, mipmaps, filtering settings, light limits, shadow-map sizes,
sampling budgets, source-cache lifetimes or rendering algorithms were reduced.
The renderer's shader/algorithm/settings sources and the texture loader are
byte-identical to 1.13.22 outside the two CPU-side idle gates. Tests compare active
sampler/diagnostic behavior with the previous path and exercise effect reloads,
visibility transitions, capture completion and concurrent overlay demand.

These checks establish less idle work, not a measured game FPS gain. They do not
attribute the reported 10 FPS difference to a particular operation.

## Costs that remain

ReShade still intercepts graphics calls and handles Present/input/state and
resource lifetimes. Stable diagnostic callbacks still incur provider dispatch.
GenericDepth retains lifecycle/binding bookkeeping needed for later activation.
Zero overhead while a hook is loaded is not guaranteed.

Texture fingerprints, replacement DDS reads and integrity checks happen during
resource creation, not on every draw of an already resident texture. The bounded
CPU DDS cache can evict files that later need another read. This can affect
loading or streaming. The integrity checks and cache size are unchanged.

Enough VRAM prevents capacity pressure; it does not eliminate texture sampling
bandwidth/cache costs. A hook-versus-PAK comparison needs the same actual texture
formats, mip chains, filtering, scene and loaded components. Engine mip/view and
streaming choices may differ when replacement happens after resource creation
arguments have already been chosen. Those differences require profiling before
changing behavior.
