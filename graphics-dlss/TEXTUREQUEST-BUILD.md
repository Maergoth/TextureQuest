# AIOV9 source and build inputs

Apache-2.0 AIO code, MIT Feeder dependency, NVIDIA interfaces and Vulkan
headers retain their separate notices. TextureQuest modifications preserve
master-off, owner-thread controls, native GUI composition, bounded capture-ring
ownership and explicit manual-depth demand. The embedded profile token still
contains 'private-v7'; it is a compatibility identifier, not a binary revision
or a license restriction. The exact V9 image is selected by its SHA256.

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
