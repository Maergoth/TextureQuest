# Lighting quality release 1.13.19

This release removes the 1.13.18 temporal shadow handoff and promotes the qualified
1.13.17 source/visibility path, alongside the 1.13.16 cache, shadow-face culling and
optional surface reconstruction work. The experimental comparison switch is gone.
Debug retains opt-in timing/capture tools. No Tensor Core backend is introduced.

## Shared quality contract

The canonical `src/quality-presets.json` defines Low, Medium, High and Max. The
renderer consumes a generated C++ representation; the installer uses the same
values. Regenerate and verify the native representation whenever the contract
changes. Do not maintain separate hand-written installer/native presets.

Low is the conservative fresh-install default; upgrades preserve existing controls
until a user explicitly selects a preset. Match the complete owned preset field
set to report its name, otherwise show Custom. Applying a preset must not change
Frame Gen, Neural Rendering, Show FPS, diagnostic recording or unrelated ReShade
settings. Every preset enables 16x AF. Exact budgets and tradeoffs are in the
[graphics guide](GRAPHICS_FEATURES.md); GPU names are engineering targets rather
than benchmark conclusions. Resolution, crowds, CPU and concurrent clients matter.

The top managed menu contains Show FPS, Frame Gen and Quality Presets. Advanced
holds individual graphical controls. Debug holds diagnostic views, captures,
statistics and logging. The core resolves optional additive ABI1 exports for
preset query/apply and separate Advanced/Debug contents. Older managed renderers
retain the existing panel fallback without falsely offering a working preset API.

## Scope and qualification

The shadow-fade rollback removes retained transition cube maps, extra snapshot
copies and dual-endpoint shading. Existing particle identity/grouping and positional
smoothing remain. Source relocation, group ambiguity, admission turnover and
shadow-map motion thresholds can still make shadows step. Neither rollback nor
promotion claims those distinct issues solved.

Qualified source paths cover the verified native particle/object profiles only.
Independent per-light shadow visibility is a correctness change and may cost more
than the old shared/dropped visibility in dense scenes. Receiver bounds and static
face culling avoid work conservatively. Surface-light smoothing remains optional
and higher tiers enable it; no texture blur or temporal history is added.

The final native images compile with `/W4 /WX` and zero C++ warnings. Focused
checks pass 38 core adapter cases, 487 production preset bridge cases, 30 diagnostics
cases and 98,664 RGB comparisons over nine software-WARP transport scenes. Five
inherited shader-filter X4000 warnings remain; no new shader warnings are introduced.
These results do not establish physical-GPU or live-game performance.

Before publication, record strict native builds, cross-runtime preset parity and
preservation checks, rollback/promotion shader checks, normal package verification
and actual packaged What's new/quality UI inspection. Record native bytes and
source hashes in their release provenance. Controlled tests cannot establish
universal in-game appearance or performance on the four hardware targets.

Textures stay 1.3.0. Publish through the normal reviewed PR/release process and
install through a fresh normal updater attempt after all EQ2 clients and launchers
are closed. Preserve data and custom values unless a preset is explicitly selected.
Do not replay a consumed local updater attempt or scan unchanged texture content.
No generator, model, scheduler or game-PAK changes belong to this release.
