# Renderer 1.13.19 lighting quality release

Built from local 1.13.18 (3842bbd0), with its 150 ms shadow crossfade and retained
history pools removed. The qualified 1.13.17 object/particle observations and
independent per-light visibility are now active without a comparison checkbox.
The existing cache, static face culling and optional surface reconstruction work
remain. This does not claim every particle-group relocation or shadow step fixed.

The shared Low / Medium / High / Max preset contract is generated from
`src/quality-presets.json` and exposed through additive ABI1 query/apply exports.
Fresh settings use Low; existing custom preferences remain until a preset is
selected. Every preset enables 16x AF. Frame Gen and Neural Rendering are outside
this contract. Advanced holds individual controls and Debug holds captures,
diagnostic views, statistics and opt-in active-path performance logging.

Final strict C++20 /O2 /MT /W4 /WX compilation completed with zero warnings.
The image is 1,271,296 bytes, SHA256
`74f53150f4767c220cf6b8f9600a26545f6ed38ed7d650a10b239865d579f9a5`.
`RELEASE-SOURCE.json` records the actual unchanged `/showIncludes` source graph,
compiler command and log pins. Its historical evidence is labelled separately.
Final qualification also passes 487 production preset-bridge checks, 30 diagnostics
checks with actual CSV output, and 98,664 RGB comparisons over nine software-WARP
lighting cases. Five inherited HLSL X4000 filter-helper warnings match the previous
fixture; no new shader warning is introduced. The D3D11 debug layer was unavailable,
so explicit binding/fallback checks were used. These checks do not establish live
EQ2 visuals or physical-GPU FPS.

Use a separate output directory and the VS18 x64 developer environment. Compile
`addon.cpp` with `/std:c++20 /O2 /MT /EHsc /W4 /WX /permissive- /DNDEBUG
/DNOMINMAX /showIncludes`, include `deps/reshade/include` and `deps/imgui`, and link
as a DLL. Rebuilding creates a new candidate; do not silently reuse old native pins.
The generated preset header must match the canonical JSON before building.

See the [release handoff](../docs/GRAPHICS-1.13.19-HANDOFF.md) and
[graphics guide](../docs/GRAPHICS_FEATURES.md). The normal updater preserves
portable data, custom settings and unchanged textures1.3.0 preparation.
