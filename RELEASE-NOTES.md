# TextureQuest 1.13.22 — Particle light baseline

Particle light strength **1× now matches 0.5× in 1.13.21**. Fresh installations use this gentler baseline.

Existing brightness is preserved automatically: saved 0.5× displays as 1×, saved 1× displays as 2×, and zero stays off. The maximum becomes 51.2× with the same physical brightness ceiling. Quality presets continue to preserve strength and other appearance preferences.

Press **Home** to show or hide ReShade settings, then select **TextureQuest**. Particle strength is under **Advanced**. The installer's **What's new** displays the bundled changelog.

This publishes the same verified installer installed locally as 1.13.22. Native migration checks, historical installer-settings compatibility, the strict native build and packaged UI checks passed. The normal local update preserved saved settings, portable texture preparation and native EQ2 INI bytes.

Texture data remains **1.3.0**. This is an app/hook update; existing texture preparation is reused. Object emission, particle radius and shadow behavior are unchanged. This release makes no new performance or native GPU-shadow stability claim.

[Full changelog](https://github.com/Maergoth/TextureQuest/blob/main/CHANGELOG.md)
