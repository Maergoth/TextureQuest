# Renderer 1.13.14 development build

Particle lights gain a small radial exclusion in their shadow depth pass.
The CPU derives a core from positive-energy triangle-centroid spread with
weighted Welford accumulation: min(0.35, 0.5 * RMS) world units. Core metadata
uses the existing elapsed-time position filter. Power, raw moments, stable
source identities and observation-based retention remain unchanged.

Static and moving shadow paths use the built position and built core together.
The depth-only single/layered pixel shaders retain geometry outside the core;
native lights use zero core. The helper changes no vertex shader, global bias,
near plane, light budget or shadow-bank layout. State is restored after drawing.
This is spatial exclusion, not recovered owner identity: another object inside
the tiny core can also be ignored. It cannot remove every self-shadow artifact.

The strict native build and 1,381 focused CPU checks passed. The corrected
hardware-D3D11 fixture passed 35 checks for static/moving/layered paths, measured
pixel position, biased depth, outside blockers, native zero-core behavior and
native state/data preservation. Initial failed assumptions and their numeric
diagnostic remain in local receipts; no historical full-map/alpha suite replay
is claimed. Actual game appearance and FPS remain unconfirmed.

The hardware probe verifies DirectX pixel SV_Position.w as view depth for these
unchanged cube shaders. See also Microsoft's documented DirectX/Vulkan position
W distinction in the [DXC options](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst#vulkan-specific-options).
RELEASE-SOURCE.json records the current 32 root native inputs and exact image.
Prior normal-map, source-stability and cache fixes remain. Dependency permission
and licenses are unchanged. See docs/GRAPHICS-1.13.14-HANDOFF.md.
