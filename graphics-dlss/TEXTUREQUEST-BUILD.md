# AIOV9 source and 1.13.6 build inputs

Apache-2.0 AIO code, MIT Feeder dependency, NVIDIA interfaces and Vulkan
headers retain their separate notices. TextureQuest modifications preserve
master-off, owner-thread controls, native GUI composition, bounded capture-ring
ownership and explicit manual-depth demand. The embedded profile token still
contains 'private-v7'; it is a compatibility identifier, not a binary revision
or a license restriction. The 1.13.6 correction changes exactly two absent
frame-generation startup defaults: the static value is false and a missing
FrameGeneration INI key falls back to 0. Explicit saved values remain on the
unchanged configuration path. The exact corrected native image is 317952 bytes,
SHA256 `96771d0ff93e8aa752702df1eac81d9affe80c5f3f1d4cc0237d3212f96d779e`.

Use a VS18 x64 developer prompt. Set SDK to NVIDIA/DLSS9981c635,
FEEDER to DLSS5-Feeder16cb1946, VULKAN to Vulkan-Headers9fbc2a78.
The tracked external directories in FEEDER are the dependency inputs; no
unversioned automatic download is needed. Their local tracked files were clean
against those commits at staging. From a separate output directory compile:

cl /LD /EHsc /O2 /MD /W4 /std:c++20 /I%SDK%/include
 /I%FEEDER%/external/reshade/include /I%FEEDER%/external/imgui
 /I%VULKAN%/include /I%FEEDER%/external/minhook/include /I<here>/addon/include
 <here>/addon/src/nr-standalone.cpp <here>/addon/src/nvof-motion-provider.cpp
 %FEEDER%/external/minhook/src/buffer.c %FEEDER%/external/minhook/src/hook.c
 %FEEDER%/external/minhook/src/trampoline.c %FEEDER%/external/minhook/src/hde/hde64.c
 /link /OUT:standalone-dlssnr.addon64 kernel32.lib user32.lib d3d9.lib
 d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib dcomp.lib

The shown command is wrapped for readability; join its lines in cmd.exe.
Historical texturequest_fg_menu_candidate001.cpp is retained by the exact author
manifest but is not a compile target. nvngx-bridge.cpp is the separate small
project-owned nvngx.dll bridge, not NVIDIA's driver. The three NVIDIA model DLLs
are release-asset payloads, never source/Git inputs. Rebuild output must not be
silently substituted for the exact published runtime SHA256.

The existing corrected build completed with exit 0 and 14 warnings in unchanged
source/dependency lines. Its two local translation units are the files shown
above. Exact current code/header/shader joins are recorded in RELEASE-SOURCE.json;
this build did not use /showIncludes, so its snapshot joins are not described as
an actual transitive include trace. Historical source, the separate bridge,
documentation and licenses retain their distinct roles.

No new GPU run or real-game behavior result is established by this two-default
change or the native compile. Existing renderer/Reflex integration limitations
remain; no driver-active, frame-generation quality or latency improvement claim
is made. All dependency licenses and notices remain included.
