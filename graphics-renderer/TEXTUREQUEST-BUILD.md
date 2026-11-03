# Renderer019 source and build

The 1.13.8 candidate image is 1,013,248 bytes, SHA256
`73e0c5e4145993a1c3818837aa18127b3c92026890c97545f59ce863c0e3167b`.
RELEASE-SOURCE.json lists the explicit addon.cpp translation unit and 34 local
headers from the strict native build's actual /showIncludes log. The frozen
45-file snapshot also contains documentation, licenses and wrappers; those
are not compiled inputs. The checkout matches the compiled roster.

The existing VS18 x64/MSVC14.50.35717 build exited 0 with:

```text
/std:c++20 /O2 /MT /EHsc /W4 /WX /permissive- /DNDEBUG /DNOMINMAX /showIncludes
/Ideps/reshade/include /Ideps/imgui addon.cpp /LD
```

Output, object and temporary files were isolated on D:. This documentation
does not rebuild the image. The generic build.cmd omits /WX; retain the strict
release flags for a new candidate and give it new image/evidence pins.
No stock game shader bytecode is distributed.

019 adds qualified constant painted-emission retention to the earlier larger
particle packets, independent particle gain/radius, 512/64 light ceilings,
lazy extra shadow tier and material-normal cache stability changes. The new
controlled GPU fixture passed four marker groups using the latest retained
native pair with owned geometry and state. The other two pairs have exact
policy/provenance checks, not new geometry execution. ROOT's closure joins
unchanged 018 components and the unchanged LIGHT_SHADER literal to retained
particle/capacity/normal cases; it does not claim a new whole-runtime test.

Sichii upstream3752e0b9 provenance and the maintainer's confirmed redistribution
permission remain intact; this does not assign an invented open-source license
to that work. ImGui/ReShade retain their separate dependency licenses.

Actual EQ2 appearance, automatic hook discovery, full beam/particle/normal
coverage and FPS remain unproved. Sun movement flicker is unresolved. PR4's
draw-state, per-light-radius and shader-layout changes require a separate
resolved build; this image does not contain PR4. See
[the candidate handoff](../docs/GRAPHICS-1.13.8-HANDOFF.md).
