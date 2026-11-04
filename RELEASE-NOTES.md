# TextureQuest 1.13.14 — Develop candidate

Particle shadow sampling gains a bounded exclusion core around the light
source: `min(0.35, 0.5 × weighted positive-emission centroid RMS)` world units.
Source metadata is smoothed, and built light position and exclusion core stay
matched across static and moving shadow paths.

The core excludes nearby cloud occluders by distance. It does not identify an
emitter's owner, exclude whole models or change global shadow bias. Unrelated
geometry inside the small core can also be ignored; geometry outside it
remains eligible to block light. This is not a universal self-shadow cure.

Focused CPU, strict build and controlled hardware checks passed. Actual EQ2
appearance and performance remain unconfirmed. Seven other native payloads, texture data
1.3.0, the bundled **What's new** history and saved controls are retained.
Develop source updates do not publish binaries or change public main/latest.
See the [canonical changelog](CHANGELOG.md) and
[graphics handoff](docs/GRAPHICS-1.13.14-HANDOFF.md) for scope and limits.
