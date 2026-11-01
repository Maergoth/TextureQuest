# Optional graphical effects

`TQEffects.fx` contains the project-authored `TQBloom` technique. It adds a subtle glow around bright pixels. It does **not** create lights, replace shadows, add AO or provide DLSS.

TextureQuest owns activation through the same Original/Upscaled shortcut used for textures. The shader has no separate key or automatic enable annotation. The controller must match both the filename `TQEffects.fx` and technique `TQBloom`; it must preserve unrelated ReShade techniques and presets.

The first version targets SDR output. It approximates SDR gamma at 2.2, extracts highlights, uses two half-resolution RGBA16F buffers and performs horizontal/vertical five-tap filtering before a bounded composite. The original alpha is retained. Strength zero and pixels with no glow return the sampled original directly. Explicit HDR color spaces and out-of-range source RGB are left unchanged rather than interpreted as SDR. Unknown presentation color space uses the SDR preview assumption. This is not a complete HDR implementation.

Defaults are conservative: strength 0.12, linear highlight threshold 0.65 and blur spacing 1.5 half-resolution pixels. All controls are bounded in shader math, including preset-loaded values. The filter uses four color samples for extraction, five taps in each blur and two samples for composition. There is no depth, motion, temporal history, game shader, external include, game data or additional texture asset dependency.

The two intermediate surfaces nominally use about 8.3 MB at 1920×1080 and 33.2 MB at 3840×2160, excluding ReShade/driver allocation overhead. This is arithmetic, not measured game usage. Disabling a technique may retain its surfaces while ReShade keeps that effect loaded.

Because this is a post-effect, bright HUD elements can glow too. It remains an optional preview until actual EQ2 checks cover HUD readability, first/third person, particles, exposure, resize, zone transitions, toggle behavior and GPU time. Source or CPU compiler validation alone does not establish ReShade FX parsing, rendering or game compatibility.

The next milestones are verified world-depth/projection and HUD handling, then spatial AO. Contact shadows require real light direction and camera data. Emissive world lighting requires material/light integration; brightness-based glow must keep its current name until that exists. See the working research document at `work/texturequest_graphics_features_20261031/EFFECTS-RESEARCH.md` in the project workspace.

This source was written for TextureQuest. No third-party shader code was copied or bundled.
