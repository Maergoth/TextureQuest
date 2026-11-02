# Exact owned ReShade provider78ef670b

Start with crosire/reshade18deaa52de0c425a78b329e9cb3c497281cd00ec
(v6.8.0) and its pinned recursive submodules. Overlay the complete final files
in overlay/ at their same relative paths. This includes the Alt-only keyboard
compatibility fix, offline GLAD selection and the GenericDepth demand protocol.
The two original patches are retained for history; do not apply the incremental
depth-demand-from-input-provider.patch directly to bare upstream. Verify the
assembled tree against FINAL-SOURCE-HASHES.json before compiling.

Build ReShade.sln /t:ReShade /m:1 /nr:false /v:minimal with Configuration=Release,
Platform=64-bit, VS18/v145, VCTools14.50.35717, SDK10.0.26100.0,
GladReproducible=true, CL_MPCount=1, UseMultiToolTask=false,
BuildInParallel=false. Disable incidental user/vcpkg imports and provide the
normal offline GLAD Python dependencies. Never fall back to network XML.
The sealed source version is6.8.0.5 because normal build attempts increment
res/version.h; the runtime's historical6.8.0.2-private label is not an exact PE
version claim. Keep the exact release image78ef670b selected by RUNTIME.json.
No provider DLL is committed here, and no official-byte-identity claim is made.
