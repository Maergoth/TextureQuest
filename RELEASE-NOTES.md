TextureQuest 1.7.0 switches Graphics hook to normal graphics-only proxy loading.

- Adds its own d3d11.dll and TextureQuest.ini beside EQ2. Existing EXE, PAKs and ReShade settings stay unchanged.
- No external injector, background watcher or sign-in task for Graphics hook. Launch EQ2 normally after installation or reboot.
- Numpad * still switches Original/Upscaled; change the key with Shortcut. Replacement textures load on demand.
- Installer status, hover help, upgrade/uninstall paths and documentation now describe this version.
- Ambiguous/unmatched resources stay original. Unchanged texture content can survive game patches; changed assets require updated mappings.

Close EQ2, uninstall the old hook, then install Graphics hook with this app. Old Memory hook removal remains under Settings. Replace PAK files remains an exclusive alternative.

Texture pack 1.2.3 and all released texture archive bytes/URLs are unchanged. Generator work is unaffected. Neither install mode is officially approved; account-policy risk is unknown.

[Technical summary and source](https://github.com/Maergoth/TextureQuest/blob/v1.7.0/docs/GRAPHICS_HOOK.md).
