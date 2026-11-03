# Standalone renderer020 source and build

The 1.13.8 candidate image is 1,015,808 bytes, SHA256
`0d092e7869d4832c0d83eb59a3cbfa096525d04ca54d55799c8fa835ae77addc`.
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

020 adds independent Particles and Objects checkboxes under Emissive lights.
Both absent defaults remain on. Disabled sources stop producer sampling and retire
only their own output; both off also stops material-normal work for emission.
Saved source controls remain independent. Exact standalone source/config gates
passed 277 focused CPU checks; the retained strict DLL was not rebuilt here.

019 added qualified constant painted-emission retention to the earlier larger
particle packets, independent particle gain/radius, 512/64 light ceilings,
lazy extra shadow tier and material-normal cache stability changes. The historical 019 controlled GPU fixture passed four marker groups using the latest retained
native pair with owned geometry and state. The other two pairs have exact
policy/provenance checks, not new geometry execution. ROOT's closure joins
unchanged 018 components and the unchanged LIGHT_SHADER literal to retained
particle/capacity/normal cases; it does not claim a new whole-runtime test.

Sichii upstream3752e0b9 provenance and the maintainer's confirmed redistribution
permission remain intact; this does not assign an invented open-source license
to that work. ImGui/ReShade retain their separate dependency licenses.

Actual EQ2 appearance, automatic hook discovery, full beam/particle/normal
coverage and FPS remain unproved. Sun movement flicker is unresolved.
This image contains no PR4 code and uses the standalone 64-byte glow layout.
The separate local PR4 integration preserves its 80-byte/per-light-far-plane ABI
and has a different DLL hash; its GPU checks do not become a standalone020 run. See
[the candidate handoff](../docs/GRAPHICS-1.13.8-HANDOFF.md).
