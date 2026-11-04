# Renderer 1.13.12 development build

030 stabilizes inferred particle source identities with deterministic world
anchor association and ownership hysteresis. Bins accelerate lookup rather
than define identity. Current RGB energy and moments remain exact; a separate
elapsed-time position filter uses a0.20second time constant. Grouping, history
and field publication commit together. Existing service-gap retention, Off,
scene reset and observation-time fade/expiry behavior remain.

The strict/W4/WX build passed with31 root native inputs, joined to source-code
commit cf775e5cc2d2a270e7b49a1d2cfd6ba501682b98. The verified renderer is
1127936bytes, SHA256
5ee82fbbc489b8de8c1bad844b8a78c2ee840f0e164d644fb5e11e38f917883a.
RELEASE-SOURCE.json records the exact source, compile-log and focused-check pins.
The seven other native payloads and texture data1.3.0 remain unchanged.

The focused fixture passed1271 policy and actual production temporal
ReadFrame/cache/EndFrame CPU checks, including boundary movement, same-group
centroid changes, zero pulses, conservation, service gaps and expiry. No new
hardware/alpha/map suite was replayed. The1.13.11 base-normal GPU result is
retained as explicitly historical component evidence, not a030 hardware pass.

Source association is spatial inference, not recovered native emitter identity.
Ambiguous split/merge/culling histories expire normally. Shadow maps retain the
existing0.1unit movement threshold and normal rebuild scheduler. Adversarial
capacity fixtures remain costly, including111.5ms for an injected32768
near-coincident old-anchor state; CPU fixture timing is not game FPS. Live
shadow stability and universal offscreen/model coverage are not established.

Develop source updates are authorized. Pushing this source does not upload or
publish the1.13.12 DLL/installer, create a release, or change public main/latest.
No binary asset upload is claimed. Existing permission and dependency licenses
remain unchanged; captured game shaders and native outputs stay outside Git.
See docs/GRAPHICS-1.13.12-HANDOFF.md for feature and package context.
