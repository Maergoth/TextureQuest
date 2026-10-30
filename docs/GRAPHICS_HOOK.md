# Graphics hook: technical summary

App 1.7.0 uses a normal **Direct3D 11 proxy plus a ReShade-based add-on**. It keeps hotkey switching while removing the external graphics injector and watcher.

1. **Installation:** offline tools read the patched stock PAKs and verified TextureQuest downloads, then build a content-fingerprint index and private replacement payload. Install adds only `d3d11.dll` and `TextureQuest.ini` beside EverQuest2.exe. Existing EXE, PAKs and ReShade settings are preserved. Conflicting proxy files are refused.
2. **Loading:** Windows loads the adjacent proxy during an ordinary game launch. Our modified ReShade 6.8.0 provider loads the private add-on through its normal add-on mechanism. The add-on loads its profile companion inside the same process. There is no external process attachment, remote-thread injection, archive redirection, startup watcher, service or scheduled task for this mode. Ordinary in-process DLL loading still occurs.
3. **Matching:** callbacks observe Direct3D texture creation, uploads and resource lifetimes. A SHA-256 fingerprint includes the texture layout and complete mip data. It matches actual stock graphics content, not a model name, archive position or game EXE hash. Missing, changed or ambiguous matches stay original.
4. **Drawing:** while Upscaled is selected, matching shader resource views are replaced around drawing, then restored. Original game resources remain valid. Resource lifetime/ownership checks prevent reuse of stale pointers. This code does not alter movement, camera, animation timing or gameplay logic.
5. **Switching:** Numpad * toggles Original/Upscaled only while EQ2 is focused. The installer can change the key without restarting the game. Replacement resources load on demand with 128 MiB CPU and 256 MiB GPU cache budgets; the game still holds its normal original resources. This avoids preloading both complete packs, but adds disk, CPU and graphics work. It is not a zero-overhead or FPS guarantee.

## Settings and lifecycle

**Graphics hook** is the default install mode. **Replace PAK files** is an exclusive alternative without a runtime graphics hook. Dual-load and Memory hook are no longer install choices; Settings retains old Memory hook removal.

Install once and launch normally, including after reboot. TextureQuest need not run. Keep its app/data/cache paths in place. Close EQ2 before installing or uninstalling. Uninstall verifies its owned files and removes only those two additions; private cache/downloads remain. If patching removed one of our files, Uninstall then Install repairs it. Changed or foreign files are preserved for review rather than overwritten.

Old injected Graphics hook users must close EQ2 and uninstall that hook first. Old Memory hook users use Settings → Uninstall old Memory hook. App updates preserve data/downloads; uninstall the active hook when prompted and reinstall afterward. Initial installation can take time and needs substantial additional private-cache disk space.

The current native runtime requires ASCII game/cache paths and a short enough cache path; installation reports incompatible paths before adding game files. Existing graphics proxies are not automatically chained. Existing ReShade.ini/RenoDX settings are left alone, but simultaneous compatibility with other graphics modifications is not promised. ReShade's general input hooks and effect runtime are disabled for this profile; TextureQuest handles its own foreground shortcut.

## Patching and coverage

An archive changing does not invalidate a texture whose uploaded content/layout stayed identical. A texture that actually changes fails the old fingerprint and stays original until a compatible mapping is prepared. New/removed assets, different upload layouts and unsupported formats can require engineering. A patcher may remove the proxy DLL; reinstall it afterward.

Pack 1.2.3 contains 24,693 named replacements. This stock-content catalog has **24,207 unambiguous named entries** and **486 ambiguous entries** that stay original. These are catalog counts, not measured unique in-game coverage. Cache pressure and unsupported/unmatched resources can leave additional textures original.

A small hardware-rendered fixture verified stock → upscale → stock switching, and a Windows EQ2 Origins in-world test confirmed the hotkey visually. That is not proof for every texture, client, patch or third-party add-on. Neither installation method has official approval; account-policy risk is unknown. Graphics-only scope is not a promise of invisibility or account safety.

## Source for review

- [Installer implementation](../src/texturequest_graphics_proxy.py)
- [Add-on and profile companion](../graphics-native/BUILDING.md): `host.cpp`, `bootstrap_proxy.cpp`, `full_pack_engine.hpp`, `full_catalog.hpp`, `proxy_files.hpp`, `proxy_addon.hpp`.
- [Modified provider overlay](../graphics-provider/README.md): texture creation/mutation/lifetime callbacks and normal-loading configuration changes against ReShade v6.8.0.
- [Runtime file hashes](../graphics-runtime/RUNTIME.json).

The package retains legacy offline-preparation and old-hook-uninstall resources. New Graphics hook installation does not start those legacy watcher/injector executables. Historical native sources remain in the repository for reviewing old versions; the runtime manifest identifies the active graphics binaries.
