# Exact owned ReShade provider a992442e

Start with crosire/reshade18deaa52de0c425a78b329e9cb3c497281cd00ec
(v6.8.0) and its pinned recursive submodules. Overlay the complete final files
in overlay/ at their same paths. FINAL-SOURCE-HASHES.json pins all 2,337
assembled source files, including final input_windows.cpp and generated
res/version.h, plus the actual compiled-input manifest. Current input retains
an original nonzero handled F10 result as well as Alt; Home and unhandled
input keep upstream behavior. GenericDepth demand/offline GLAD remain intact.
The new overlay-demand header, event registration/export integration and one
runtime GUI admission predicate are the only runtime changes from 6.8.0.6.

Build ReShade.sln /t:ReShade /m:1 /nr:false /v:minimal with Configuration=Release,
Platform=64-bit, VS18/v145, VCTools14.50.35717, SDK10.0.26100.0,
GladReproducible=true, CL_MPCount=1, UseMultiToolTask=false,
BuildInParallel=false. Disable incidental user/vcpkg imports and supply the
normal offline GLAD Python dependencies; never fall back to network XML.

The normal one-worker build exited naturally with code0. It retains upstream
and bounded diagnostic warnings and is not a /W4 /WX provider build. Normal
MSBuild increments the version header; the sealed post-build source is
6.8.0.7 UNOFFICIAL. RUNTIME.json retains historical6.8.0.2-private as a
compatibility token and selects the exact 5,412,864-byte image with SHA256
`a992442ed648d1c0984bfd34bfc3426f91dbe8c12d4211515e7358af8e0c20e6`.
Rebuilding creates a new candidate rather than reproducing old evidence.

Original incremental patches remain historical evidence. Do not apply the
depth-demand patch directly to bare upstream. No provider DLL is committed
here and no official-byte-identity claim is made. Exact-fragment and owned
window tests establish the conditional menu mechanism; actual EQ2 F10 cure
remains unconfirmed. Licenses and notices remain unchanged.

The overlay-demand change passed 96 focused assertions using the production
registry and extracted registration/export/GUI admission functions, plus
400,000 concurrent demand toggles, 800,000 concurrent reads and 50,000
registration lifecycle cycles. That fixture passes /W4 /WX; the complete
normal provider build retains its upstream warnings. The exact DLL exports
`TextureQuestSetOverlayCallbackDemandV1` and the previous depth-demand APIs.
Actual compiler read tracking records 365 source/header inputs and 151
translation units. No in-game FPS improvement has been measured by these checks.
