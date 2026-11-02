# Renderer017 release source and build

The 1.13.6 renderer image is 1005056 bytes, SHA256
`f5e6bf98b073d13ed594e006fd2396595aa8dcdc95841a389f31b25487617943`. RELEASE-SOURCE.json lists the 36 actual
local compilation inputs: addon.cpp and the headers recorded by the existing
strict full build's /showIncludes log. The frozen snapshot contains
44 files; its documentation, release metadata and build wrappers are
not described as compiled inputs.

The integration retains Sichii upstream3752e0b9 provenance and the maintainer's
confirmed redistribution permission. This does not assign MIT/Apache terms
to Sichii's work. ImGui/ReShade dependency licenses retain their own terms.
The upstream README remains preserved.

The release build used the existing VS18 x64/MSVC14.50.35717 toolchain and:

```text
/std:c++20 /O2 /MT /EHsc /W4 /WX /permissive- /DNDEBUG /DNOMINMAX /showIncludes
/Ideps/reshade/include /Ideps/imgui addon.cpp /LD
```

The preserved release command uses an isolated output/object/temp directory.
The generic build.cmd wrapper omits /WX; add it for the release warning policy.
No rebuild or test was run while sealing this existing exit0 build. Rebuilding
produces a new candidate; use the exact runtime descriptor/release image pins
for publication rather than substituting a new binary under old evidence.

017 includes surface-saturation restoration, the material/normal attempt,
emissive limits1–256/particle limits1–32 with absent defaults64/4 and native-lamp
priority in the shared Shadowed lights pool, finite particle-primitive exclusion,
Sun border sampling guards and both softness UI scales1–10. Stored appearance
is preserved until a softness control is adjusted. Invalid Sun gather lanes
contribute no shadow and the original tap count remains. Alpha/radial behavior
is retained from1.13.5 rather than a new correction.

GPU closure is separate. Normal coverage is partial; Sun movement flicker is
unresolved and histories are diagnostic. No full in-world visual, particle
coverage or performance result follows from this build. See
[the collaborator handoff](../docs/GRAPHICS-1.13.6-HANDOFF.md).
