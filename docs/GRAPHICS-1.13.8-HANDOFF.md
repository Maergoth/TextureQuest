# TextureQuest 1.13.8 graphics candidate

This batch is being prepared as a separate PR against current public main.
Keep that PR open and unmerged for review. Sichii's draft PR4 remains untouched;
the separate local integration branch combines both changes for a normal
portable-app build with a different renderer binary. No new public release or local installation is included in
this preparation. The generator remains paused.

## Changes

- Emissive lights has separate Particles and Objects checkboxes. Both absent
  defaults remain on; disabling one stops its source sampling and retires only
  its owned output. Both off also stops emission material-normal work. Advanced
  settings stay saved independently and are greyed out for a disabled source.
- Complete particle packets now allow 512 draws and 32,768 triangles. The
  captured combat workload contained 331 supported draws and 24,212 triangles,
  exceeding the old draw bound. This increases capacity without accepting an
  incomplete packet as complete.
- Particle-light strength and radius have independent controls. An absent
  particle strength inherits the previous shared strength once; later surface
  strength changes do not change it. Radius controls lighting reach and caster
  work independently of brightness.
- Emissive and particle ceilings are 512 and 64. Absent defaults stay 64 and 4;
  these still share the Shadowed lights pool, with native lamps first. The
  additional 256 shadow-map slots allocate only when used and can add 384 MiB
  of GPU memory. Higher limits cost work and do not guarantee source coverage.
- Painted emission kept applies a constant retained share to strictly matched
  native additive world passes. Optional measurement failure, emitter selection
  and the camera-dependent light mask no longer choose that share. Unknown,
  disabled and Original paths retain native output.
- The supported material-normal cache allows 512 current-frame draws. A better
  material pass can replace its base record at capacity, avoiding rejection
  based only on the cache already being full. Unsupported paths retain fallback.
- The provider retains an original keyboard hook's nonzero handled F10 result,
  preventing the demonstrated redundant default-menu path. Home, existing Alt
  handling and unhandled F10 behavior remain intact.
- A normal graphics refresh applies the selected Graphical Effects group to
  every available bundle. Legacy effects-only flags can no longer silently omit
  available DLSS support. Textures-only selections keep Graphical Effects off;
  missing profiles remain unavailable. Unknown capability status or stale saved
  choices stop before setup instead of clearing the selection. Loading
  preferences is read-only.

Texture data remains 1.3.0. The AIO/DLSS, frame-generation, super-resolution and
neural-rendering native payloads are unchanged. Existing in-game feature
opt-outs and Home remain separate from the installer component group. The
1.13.7 unchanged-texture preparation fast path is retained.

## Evidence and remaining checks

The grouped-refresh change passed 33 focused headless tests, including legacy
selections, missing/incomplete capability status, compare-and-save refusal and
no writes to synthetic feature settings. The provider's exact input fragment
and an owned Windows message loop reproduced the conditional F10 menu path;
they do not establish the result in EQ2. The exact standalone020 strict renderer build passed, with 277 focused
CPU/config/retirement checks. No new standalone020 GPU execution was performed.
The earlier019 GPU evidence below remains historical.
The new painted-kept GPU fixture passed four marker groups on the latest
retained native pair with controlled geometry/state, including measurement
refusals, alpha, feature-off gates, lifetime and native state/constant bytes.
The other two known pairs have policy/provenance checks, not new geometry
execution. ROOT's closure joins unchanged earlier particle/capacity/normal
components and the unchanged light shader; no whole-runtime rerun is claimed.
Exact joins are recorded in component RELEASE-SOURCE metadata/runtime provenance.

Actual game behavior, the reported moving-view flicker, complete beam/particle
coverage and FPS are not proved. Material normals still have partial coverage;
Sun movement flicker remains a separate unresolved issue. Controlled cases do
not establish every native callback or scene.

This PR contains the standalone020 renderer (DLL0d092e78…), without PR4 author
code. The separately resolved local integration includes PR4 and our fixes, uses
its 80-byte glow/per-light far-plane ABI (DLL554bb0ab…), and passed its own five
controlled GPU marker groups. Those results belong to that different binary.
Keep both branches independently reviewable; neither should overwrite the other
author's branch. No actual game appearance or FPS proof is implied.

Keep local captures and stock game binaries out of the repository. Install the
combined candidate only through the normal app/hook flow after the games are
closed; preserve portable data and settings.
