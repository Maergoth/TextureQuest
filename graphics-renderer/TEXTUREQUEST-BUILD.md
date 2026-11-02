# Renderer014 release source

This directory preserves the 41 exact source inputs for EQ2Graphics.addon64
92df3b0d7e365161c09e9a7528e601abfb7f0c72e763499d5056d9f0b395edb6.
The source is the private TextureQuest integration of Sichii's upstream3752e0b9.
The maintainer confirmed redistribution permission on2026-11-02. This record
does not assign MIT/Apache terms to Sichii's work. ImGui/ReShade licenses in
deps retain their own terms.

Run build.cmd from a Visual Studio C++ developer environment. The release
build used VS18/v145 x64, /std:c++20 /O2 /MT /EHsc /W4 /WX /permissive-
/DNDEBUG /DNOMINMAX and the bundled deps/reshade/include +deps/imgui headers.
The upstream wrapper omits /WX; adding /WX reproduces the release warning
policy. No game/capture/user-data inputs or runtime DLLs are in this source tree.
Rebuilding does not imply a byte-identical compiler output. The release asset
must match RELEASE-SOURCE.json and the separately published runtime descriptor.
