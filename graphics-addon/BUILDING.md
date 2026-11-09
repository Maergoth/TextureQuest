# Building the TextureQuest add-on

The 1.13.15 core image was built with VS18 x64/MSVC14.50.35717 from
this release source. Its 475648-byte image is SHA256
`26521272e3b27aff0362a28ea3957b006d5f6b4615eaa5fa6366abb6923db166`.
`RELEASE-NATIVE-SOURCE.json` records the actual local translation units and
headers captured by `/showIncludes`. The core no longer instantiates or calls
Reflex driver control, and its menu exposes only optional FG/NR controls.
Historical low-latency helper files are not compilation inputs.

For a separate rebuild, use an x64 developer prompt and an isolated output
directory. The release command used `/Brepro /std:c++20 /O2 /MT /EHsc /W4
/permissive- /DNDEBUG /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /showIncludes`,
the include directories `deps/reshade/include`, `deps/xxhash`,
`deps/libdeflate`, `deps/imgui`, and these translation units:

- addon.cpp
- deps/libdeflate/lib/deflate_decompress.c
- deps/libdeflate/lib/zlib_decompress.c
- deps/libdeflate/lib/adler32.c
- deps/libdeflate/lib/utils.c
- deps/libdeflate/lib/x86/cpu_features.c

Link a DLL with `user32.lib bcrypt.lib`. Generic `build.cmd` also builds the
retained selfchecks; those executables are not part of the published add-on.
A rebuild is a new candidate and must not silently replace the exact release
image or reuse its review/runtime pins.

`DDSReader` keeps immutable session bytes only after full Windows BCrypt SHA256
admission. A per-reader mutex serializes misses; the LRU owns at most 16 entries
and 64 MiB. Each read still checks dimensions, mips and pixel bounds. A thread's
returned bytes remain pinned until its next Read even across eviction; those
active pins can temporarily exceed the cache-owned limit. Eviction or a new
reader rechecks disk identity, size and SHA. No disk cache or worker is added.

The dedicated native CPU/filesystem fixture passed 185 checks with 47 actual
instrumented Reader SHA calls. Physical symlink creation was denied by Windows
1314 and is explicitly skipped; the separate injected reparse-attribute branch
passed. These are correctness checks, with no measured EQ2 FPS/load-time claim.

Runtime lookup remains XXH3-128 and the loose map remains TQMAP002. See
[the DDS contract](../docs/LOOSE_DDS.md). ReShade add-on headers, xxHash and
libdeflate retain their respective dependency licenses under `deps/`.

The release's separate ReShade provider is the retained custom depth-demand
build, not an unmodified official installer DLL. Its unchanged 5410816-byte
image is SHA256 `78ef670bd7baf3688f8570294cf793050f9fd8f2751fa7b3086c93ae5eb14fd1`;
provider source/build provenance is under `provider/`. Native payloads are
release assets; `graphics-runtime/RUNTIME.json` selects their exact bytes.
