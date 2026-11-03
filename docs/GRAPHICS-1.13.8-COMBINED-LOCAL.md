# 1.13.8 combined local source handoff

This branch combines Sichii's draft PR4 head
`584f0b5e7a9815bb3c40a1f5dc27139cd6cc6c13` with the 1.13.8 fixes reviewed in
[PR6](https://github.com/Maergoth/TextureQuest/pull/6), whose reviewed head is
`89bc12e9e447594a8044fb9dc9b7db75adc7f8bb`. Author PR4 remains untouched.
The integration started from merge `7d41180e82d3894a19b6c1866b449dc872f64c4d`
(public main a5df617 plus that exact author head). Later working-tree changes
were the actual packaged inputs; that starting commit alone does not identify
the combined code. The final integration commit records those resolved files.

The port retains PR4's BoundDraw state, per-light radii/FarPlanes, shader ABI,
bindings and instanced paths. It includes complete larger particle packets,
independent particle gain/radius, larger light ceilings/lazy map tier, qualified
constant painted retention, separate Object/Particle gates, supported normal
cache changes, handled F10 retention and grouped installer selection. Legacy
point-filter behavior and saved Sun softness remain until deliberately changed;
the app does not seed PointShadowSoftness over an existing legacy ShadowSoftness.

The exact local package is package003 in comparison/combined1138-package-001:

- ZIP: 227,946,035 bytes, SHA256 `1f9e164902e09fe6f9005bb72456b2ed42a0c0a20f46b48aec31107de7990654`.
- EXE: 5,996,609 bytes, SHA256 `6a045fd74b5b6e6f46a989c1a2aa75ba5de45744828b3b4b5a78730f47464c43`.
- Renderer: 1,041,408 bytes, SHA256 `554bb0ab2d48a35a808b061694f825866410a2f1c4e1fd9b3ef862a5bbe6e53a`.
- Provider: 5,410,816 bytes, SHA256 `af45413c73384ec412930efd2daba2523f8d04003ff8e89d2cf86e1141ab7d9e`.
- Runtime descriptor SHA256 `1944f780675dd9e25168a5d9633e0100028afb7cd61ce0d37548696255796a6f`.

The other six native payloads and texture data1.3.0 remain unchanged. Normal
build, exact source/runtime/ZIP checks and a D-only frozen UI smoke passed.
The legacy-default migration fixture passed. ROOT's corrected combined GPU run
exited0 naturally with all five markers; the earlier reflection-fixture failure
is preserved separately. These controlled results do not prove every game
callback, beam/particle/normal coverage, F10 cure, appearance or FPS. Sun movement
flicker remains unresolved. No installation result is claimed by this handoff.

Root texturequest-version.json and SHA256SUMS now describe this combined ZIP.
They are source/distribution metadata, outside the packaged managed tree and
build source inventory. The normal local installer uses the compiled1.13.8
version, BUILD-MANIFEST and exact ZIP inventory; the child ACK and hook refresh
use that compiled version. Synchronizing these two files and this document does
not change the already-built executable, runtime or source joins. No rebuild is
needed. This is a local candidate, not a public release or author-branch update.
Generator remains paused; installation/reopening stays with the normal operator
flow and a fresh closed-game check.
