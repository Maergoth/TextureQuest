# Local lighting candidate 1.13.16

This candidate is based on public 1.13.15 (`8ae625d`). It is not a public release.
The installer bundles the same changelog used by developers. Existing choices,
the public conservative defaults, texture version 1.3.0 and the seven other
native payloads are retained.

Object measurements now update observed 2-unit world cells. Parents and the
whole emitter contain integrated known RGB divided by their full geometric
area, rather than colours inferred from whatever happens to be visible. This
prevents an unknown sibling inheriting the newest parent colour and adding
unobserved energy as the camera changes hierarchy levels. Measured darkness
counts as a real observation. Unseen known cells retain their last observation.
Sparse first observations can initialize local cells; replacing an established
colour still requires four valid texels. Sampling starts once geometry is ready.
Monotonic readback sequences supplement the existing lifetime/type guards.
A tiny, bounded mapping correction admits a unique adjacent geometry cell when
reprojection rounds a sample across its grid boundary. Ambiguous neighbors and
larger gaps remain unobserved; this does not restore broad colour fallback.

Static cube shadow maps also reject meshes whose known bounds are wholly
outside a face. Unknown or ambiguous bounds keep the conservative fallback.
Resolution, sample counts, update scheduling, moving shadows, brightness and
light limits are unchanged. Modeling the two supplied capture geometries found
about 15â€“17% fewer eligible face submissions; that is not measured game FPS.

The default reconstruction rejects invalid samples and normal-incompatible
fallbacks. Advanced includes an optional **Smooth emissive lighting** control,
off by default. Its separate shader uses the same four reduced-light texels
with slope-aware weights only on coherent, low-contrast surfaces. It does not
filter the game's texture colour, keep frame history or smooth source animation.
It costs extra arithmetic, so the wider nine-tap experiment was not adopted.
Controlled software tests found reduced low-amplitude grain while preserving
the tested depth, normal, colour, silhouette and high-contrast lighting edges.

Limitations: this is a bounded cache correction, not proof that every reported
white-to-orange change shares one cause. A partial view inside one 2-unit cell
can still change that cell's measured mean. Low-support observations may keep
their initial colour until a stronger view. Previously unobserved geometry is
not assigned guessed light. Unsupported or game-culled native sources remain
outside the hook's observations. Existing AO checkerboard history and sun
direction issues are separate findings; they were not silently rewritten here.

Native provenance records the compiled inputs and image. Focused evidence is
retained under `work/texturequest_lighting_20261110` in the operator workspace.
Software-renderer timings are not GPU or game benchmarks. The optional filter
and cache change still need the same-scene EQ2 comparison before publication.
