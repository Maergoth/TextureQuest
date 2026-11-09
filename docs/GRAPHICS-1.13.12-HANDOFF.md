# TextureQuest 1.13.12 local handoff

The maintainer authorized pushing this source to Develop after the local build
was verified. Main and public binary releases remain unchanged; this source
push does not publish the private1.13.12 installer or renderer asset.
Texture data stays1.3.0. The seven
non-renderer native payloads, saved controls and existing shadow banks stay at
the installed1.13.11 versions. Generation remains paused.

Particle source identities no longer come from floor(centroid/4). Deterministic
nearest-anchor association and ownership hysteresis keep inferred identities
across those boundaries. World bins accelerate lookup; they do not define the
source identity. Each accepted triangle contributes its integrated RGB energy
and moments once, including observed zero-power geometry. Current energy is not
temporally averaged, preserving native fire animation and immediate dark pulses.

The emitted position is separate from the exact observed moments. An elapsed
time filter with0.20second time constant moves it toward the current centroid;
allocation and light updates both use that position. New sources start at their
final centroid. Publication of grouping, retained state and field is atomic.
Scene changes and Off clear the state. Service gaps preserve committed keys.
The existing2second retention plus1second fade still uses observation time.

Identity is spatial inference, not recovered native emitter identity. Ambiguous
split/merge or native-culling histories expire normally rather than deleting an
unseen nearby light by guesswork. Newly culled sources cannot be discovered.
Shadow maps retain their0.1unit movement threshold and normal rebuild scheduler;
no unbuilt pose is committed. This reduces source jumps without claiming a
continuous per-pixel shadow update or a universal stability fix.

Independent source review passed. The isolated fixture passed1,271 focused
policy and actual production ReadFrame/cache/EndFrame checks, including boundary
oscillation, same-group centroid movement, zero pulses, RGB conservation,
service gaps and expiry. The integrated renderer passed a strict/W4/WX build.
Representative host CPU grouping timing was0.906ms versus2.918ms for the prior
path. At32,768triangle capacity, sparse/churn cases cost13.9/27.9ms; a deliberately
injected32,768near-coincident old-anchor state cost111.5ms. These are CPU fixture
measurements, not game FPS or guaranteed bounds. No unchanged hardware/alpha/map
suite was replayed. Live visual approval remains outstanding.

Evidence is in comparison/stable-particle-groups-030-001 and local11312.
Normal package verification and any later installation are recorded separately
in local11312/VERIFIED.json and update-001/COMPLETION.json.
