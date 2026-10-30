TextureQuest 1.5.0 adds full-pack Graphics hook installation with live Original/Upscaled switching.

- Graphics hook is the default for fresh installations. Replace PAK files and Memory hook remain exclusive alternatives, with hover explanations.
- Press Numpad * while EQ2 is focused to switch quality. Use the Shortcut button beside Dual-load to choose another key or combination; an installed shortcut can change without reinstalling or restarting EQ2.
- Dual-load keeps a bounded comparison cache warm. Leave it off for on-demand Original comparisons and lower retained memory. Both settings support the hotkey.
- The full released pack has passed an in-world test on Windows EQ2 Origins, including live switching. Other client versions and future patches have not been fully tested.

Graphics hook combines the named-file loader with a graphics comparison layer. It leaves game PAKs, settings and shortcuts untouched, installs a per-user background watcher, and requires stock game archives when preparing its private cache. Original comparison textures are disk-backed with 128 MiB CPU and 256 MiB GPU comparison budgets. Ambiguous fingerprints (42 entries in this pack) keep the correct upscale instead of guessing an Original; cache limits can also leave some textures upscaled. This is not a second copy of the entire pack in VRAM.

Close EQ2 before installing, uninstalling or updating the app. An already attached game retains its current components until it closes. Launch normally after installation reports ready. After a patch changes game archives or indexes, uninstall and reinstall the hook to refresh its private cache; automatic refresh is unfinished. Runtime injection has unquantified compatibility/account-policy risk and is not officially approved.

Get app update still downloads, verifies, installs and restarts the portable app while preserving data. Hook users are prompted to uninstall their owned hook first; close attached clients, then reinstall after updating. Existing older apps may require manual hook removal before their update button enables.

This is an app-only release. Textures remain 1.2.3: all 24,693 replacements in 310 archives, including recovered Maj'Dul/Desert of Flames, have unchanged bytes and URLs. Unfinished generation is not included. No generation recipe, GPU settings or scheduling changes are included.
