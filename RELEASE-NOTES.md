<<<<<<< HEAD
TextureQuest 1.10.0 private preview

- Textures (required), optional Graphical Effects, and an unavailable DLSS choice.
- First effect: subtle glow/bloom, off by default. AO/shadows/scene lighting pending.
- Your custom key toggles every selected TextureQuest component together.
- Saved choices per game; Apply changes reuses the texture cache.
- Reversible ReShade FX settings; user presets and external edits preserved.
- Includes released 1.9.3 automatic owned-hook refresh and shortcut fixes.
- New app versions reapply hook/settings automatically, even for unchanged DLLs.
- Includes released 1.9.4 metadata-only update checks and concurrent results.
- Needed downloads start during cache comparison; full verification stays in installation and explicit Verify.

This candidate is for a private EQ2 test, not public release. Close EQ2 before
installing or applying choices. Enabling glow also loads enabled effects from
your ReShade preset. No native EQ2 graphics settings or game archives are edited.
=======
TextureQuest 1.9.5

- Checking for updates leaves your working graphics hook installed.
- App updates take priority over texture setup.
- Setup prepares and verifies the replacement before removing the old hook.
- If a newer app release appears after you checked, the updater refreshes the target automatically.
- Preparation failures keep the existing hook and saved textures intact.

The app restarts after updating and preserves data. Texture data remains 1.3.0;
this release does not change the graphics runtime or add experimental effects.
>>>>>>> f5c2b1acb681a665f17e68562524919619bc603c
