# TextureQuest 1.13.23 — Lower idle hook overhead

Remove unnecessary background work while preserving texture and lighting quality:

- When **Show FPS** is off, skip FPS sampling and let ReShade avoid an otherwise unnecessary GUI frame. Home, other add-ons' overlays and ReShade messages/statistics remain available.
- Stop repeated searches for owned effects after they are confirmed disabled or absent. Reloads and setting changes invalidate that result; active effects still use fresh handles and depth readiness.
- Skip capture-only device queries when no capture is active. In the optional renderer, skip inactive sampler work and idle diagnostic frame collection.
- Keep callback registration stable across toggles, and serialize FPS visibility changes across runtimes.

**No quality settings were reduced.** Texture bytes, mipmaps, filtering quality, light/shadow budgets, rendering algorithms and saved controls are unchanged. This publishes the same verified installer already installed locally as 1.13.23.

The native builds, focused lifecycle/concurrency checks, active-path comparisons, source invariants and packaged UI checks passed. The normal local update preserved saved settings, texture preparation and native EQ2 INI bytes. These checks establish less idle work; they do not establish an in-game FPS gain, explain the entire reported 10 FPS difference or promise zero ReShade overhead.

Press **Home** to show or hide settings, then select **TextureQuest**. The installer's **What's new** displays the bundled changelog. Texture data remains **1.3.0**; existing texture preparation is reused.

[Full changelog](https://github.com/Maergoth/TextureQuest/blob/main/CHANGELOG.md) · [Implementation and remaining costs](https://github.com/Maergoth/TextureQuest/blob/main/docs/IDLE_HOOK_OVERHEAD.md)
