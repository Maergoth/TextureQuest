TextureQuest 1.9.2

- Fixes automatic upgrades from older apps, including 1.4.2, that reported
  "Bundled texture/release versions disagree".
- Home is the only configurable ReShade shortcut left enabled.
- Screenshot, effect, preset-switch and referenced preset hotkeys are disabled.
- Your custom TextureQuest toggle stays unchanged (Numpad * by default).
- Settings → Reset ReShade shortcuts reapplies these defaults without deleting textures.
- Keeps texture data 1.3.0, the existing graphics renderer, downloads and settings history.

Close EQ2 before finishing the upgrade, then reopen it normally. Keep the data
folder in place. F10/Alt+3 were not assigned in the inspected ReShade config;
their reported freeze/tone has not yet been confirmed resolved in game.
Unfinished DLSS/effects work is not included in this hotfix.
