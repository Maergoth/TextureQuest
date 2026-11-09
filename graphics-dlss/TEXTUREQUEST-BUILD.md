# AIO source and 1.13.15 build inputs

Apache-2.0 AIO code, MIT Feeder dependencies, NVIDIA interfaces and Vulkan
headers retain their notices. Compatibility profile names such as private-v7
remain identifiers, not version or license claims. FG and NR start off when
settings are absent. Explicit FG/NR settings remain supported. SR is disabled
at startup, mailbox normalization, feature creation and evaluation; a saved SR
request cannot reactivate it. Feature recreation permits a null SR handle.
The new image is 314368 bytes, SHA256
`867747ff65d70113f8f810cc3505fa41f7181bd21017f78195663c5d7d8cf188`.

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

The 1.13.15 build completed with exit 0 and 14 warnings in unchanged AIO and
MinHook lines. `/showIncludes` captured the actual project include graph; exact
source/header/native pins are in RELEASE-SOURCE.json. Its source targets remain
the two local C++ units and the listed MinHook C files. Historical candidate
source and the separate bridge are not this image's compile targets.

Focused CPU mailbox policy checks passed. No new GPU/model run or game benchmark
is claimed for feature retirement/default changes. All dependency notices remain.
