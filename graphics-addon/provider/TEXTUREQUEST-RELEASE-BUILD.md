# Exact owned ReShade provider af45413c

Start with crosire/reshade18deaa52de0c425a78b329e9cb3c497281cd00ec
(v6.8.0) and its pinned recursive submodules. Overlay the complete final files
in overlay/ at their same paths. FINAL-SOURCE-HASHES.json pins all 2,336
assembled source files, including final input_windows.cpp and generated
res/version.h, plus the actual compiled-input manifest. Current input retains
an original nonzero handled F10 result as well as Alt; Home and unhandled
input keep upstream behavior. GenericDepth demand/offline GLAD remain intact.

Build ReShade.sln /t:ReShade /m:1 /nr:false /v:minimal with Configuration=Release,
Platform=64-bit, VS18/v145, VCTools14.50.35717, SDK10.0.26100.0,
GladReproducible=true, CL_MPCount=1, UseMultiToolTask=false,
BuildInParallel=false. Disable incidental user/vcpkg imports and supply the
normal offline GLAD Python dependencies; never fall back to network XML.

The normal one-worker build exited naturally with code0. It retains upstream
and bounded diagnostic warnings and is not a /W4 /WX provider build. Normal
MSBuild increments the version header; the sealed post-build source is
6.8.0.6 UNOFFICIAL. RUNTIME.json retains historical6.8.0.2-private as a
compatibility token and selects the exact 5,410,816-byte image with SHA256
`af45413c73384ec412930efd2daba2523f8d04003ff8e89d2cf86e1141ab7d9e`.
Rebuilding creates a new candidate rather than reproducing old evidence.

Original incremental patches remain historical evidence. Do not apply the
depth-demand patch directly to bare upstream. No provider DLL is committed
here and no official-byte-identity claim is made. Exact-fragment and owned
window tests establish the conditional menu mechanism; actual EQ2 F10 cure
remains unconfirmed. Licenses and notices remain unchanged.
