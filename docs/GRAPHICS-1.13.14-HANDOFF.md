# 1.13.14 particle shadow candidate

This candidate starts from installed 1.13.13. Only the renderer native payload
is being changed; the other seven native payloads, texture data 1.3.0 and saved
controls are retained. Generation remains paused. Develop source publication
does not publish a binary release or change public main/latest.

The new particle-only radial depth exclusion uses
`core = min(0.35, 0.5 Ã— weighted positive-emission centroid RMS)` in world
units. Source spread metadata is smoothed with the representative position.
Each static map commits the particle position and core together; moving shadow
paths use those built values rather than a newer, unbuilt core.

This is a bounded spatial rule for near-cloud occluders, not owner identity,
a global depth bias or whole-mesh exclusion. Unrelated geometry inside the
small core can also be ignored. Walls, distant geometry and other characters
outside the core remain eligible blockers. Native culling, inferred source
ambiguity and unsubmitted particles remain limitations; this does not establish
a cure for every model seam, particle self-shadow or lighting artifact.

The focused CPU sequence passed1,381 checks. The final strict native build and
corrected hardware-D3D11 source-core fixture passed35 checks, including actual
pixel-position reconstruction under depth bias, static and moving paths,
layered cube selection, outside blockers, native zero-core behavior and native
state/data preservation. The first failed shader assumption and diagnostic are
retained in local receipts; the current production shader uses the measured
DirectX pixel-position convention. No historical full-map/alpha suite replay
is claimed. Actual EQ2 appearance and performance remain unconfirmed.

Normal package and installation status are recorded separately in local11314/
VERIFIED.json and update-001/COMPLETION.json. Older update actors are consumed
or superseded and must never be replayed.
