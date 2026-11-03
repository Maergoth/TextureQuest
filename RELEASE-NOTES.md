TextureQuest 1.13.8 graphics candidate

- Increase complete particle capture to 512 draws / 32,768 triangles for the captured combat workload, with independent particle-light strength and radius controls.
- Raise emissive/particle ceilings to 512/64, retaining absent defaults 64/4 and native-lamp priority. The extra map tier allocates on demand and can add 384 MiB of GPU memory.
- Keep a constant painted-emission share on strictly qualified native world passes, independent of optional measurement and the camera-dependent light mask.
- Increase supported material-normal replay capacity to 512 and allow better material passes to replace base records at capacity.
- Retain an original keyboard hook's handled F10 result, addressing the controlled redundant Windows-menu path while preserving Home and existing Alt behavior.
- Include every available bundle when a selected Graphical Effects group is normally refreshed, fixing legacy effects-only flags that could omit the DLSS panel. Unknown capability status refuses before setup.

This is an unmerged review candidate, separate from Sichii's draft PR4. A local
PR4+fixes build requires a reviewed semantic port and a new build of the actual
resolved source. No new public release is being published by this preparation.

Strict renderer compilation, targeted controlled GPU cases, owned input-window
checks and 33 focused app tests passed. Actual EQ2 appearance, automatic hook
discovery, full beam/particle/normal coverage, F10 cure and FPS remain unproved.
Sun movement flicker remains unresolved. See
[the collaborator handoff](docs/GRAPHICS-1.13.8-HANDOFF.md) for scope and limits.

Texture data remains 1.3.0. AIO/NVIDIA/main texture-add-on bytes are unchanged;
existing feature opt-outs and absent frame-generation-off defaults remain.
The generator stays paused. Preserve portable data/settings and use the normal
install flow after games are closed. No stock game binaries or shaders ship in
the source repository. Third-party permissions, licenses and notices remain
intact; historical runtime IDs/private labels are compatibility tokens.
