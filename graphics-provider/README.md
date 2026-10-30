# TextureQuest graphics provider

This directory contains the source overlay used by the graphics-only provider in app 1.7.0, against **crosire/reshade v6.8.0**. `OVERLAY.json` records each changed/new file and its upstream hash where present. `overlay/` includes the complete changed files, not generated objects or dependency binaries.

On Windows with Visual Studio C++ desktop tools and the appropriate Windows SDK:

```powershell
git clone --branch v6.8.0 --recurse-submodules https://github.com/crosire/reshade.git reshade-tq
Copy-Item -Path overlay\* -Destination reshade-tq -Recurse -Force
MSBuild reshade-tq\ReShade.sln /t:ReShade /p:Configuration=Release /p:Platform=x64
```

Use the resulting x64 ReShade64.dll as `d3d11.dll` in the TextureQuest graphics runtime. Build the add-on/companion using [these instructions](../graphics-native/BUILDING.md), then regenerate the runtime manifest from the complete new binary hashes. Do not mix an unmodified provider with this add-on: the creation/mutation interfaces differ. Toolchain/dependency changes can produce different bytes; the supplied runtime hashes describe the tested release, not a reproducible-build guarantee.

The overlay adds explicit resource-lifetime/creation/mutation APIs and D3D11 integration. Its startup uses the separately owned TextureQuest.ini to locate the private profile; it does not overwrite a user's ReShade.ini. Generic input hooks are disabled. Normal ReShade module loading still loads the provider/add-on inside EQ2; there is no remote-process injector in this mode. See the [technical summary](../docs/GRAPHICS_HOOK.md) and license.
