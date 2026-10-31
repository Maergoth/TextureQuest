# Building the TextureQuest add-on

Requires Visual Studio 2022 Build Tools with the C++ workload. Run `graphics-addon\build.cmd`; output goes to `graphics-addon\out\`.

- `TextureQuest.addon64` — copy to `graphics-runtime\` and regenerate `graphics-runtime\RUNTIME.json` (file hashes).
- `selfcheck.exe <fixture dir> [map.bin downloads-dir]` — map, VPK and layout checks. Create the fixture with `python tests\fixtures\make_addon_fixture.py graphics-addon\out\fixture`.

Vendored, unmodified: ReShade 6.8.0 add-on headers (BSD-3-Clause), xxHash 0.8.3 (BSD-2-Clause), libdeflate 1.24 (MIT). Licences are next to each dependency under `deps\`. xxHash and libdeflate licences are also shipped under `graphics-runtime\licenses\`; ReShade's licence is shown on its About page.

`graphics-runtime\ReShade64.dll` is the official ReShade 6.8.0 add-on build, unmodified, taken from `ReShade_Setup_6.8.0_Addon.exe`.
