TextureQuest 1.13.6

- Cache SHA-verified loose DDS session bytes within a 16-object/64 MiB reader cache, avoiding repeated disk reads and full hashes on cache hits.
- Restore optional world-surface saturation controls and attempt supported material/normal routing. Normal coverage remains partial.
- Share compiled shader results between lighting and material normals; material setup returns immediately while compilation is pending.
- Add tunable emissive and particle light limits: 1–256 and 1–32, with absent defaults of 64 and 4. They share the Shadowed lights pool; native lamps take priority.
- Keep finite remote or oversized particle primitives from invalidating the whole emission packet and suppressing nearby light. Existing alpha and radial behavior is retained.
- Exclude invalid Sun gather lanes from shadow contribution while retaining the original tap count, treating unavailable samples as unshadowed. Controlled border cases pass; Sun movement/shadow-direction flicker remains unresolved.
- Present both shadow-softness controls on a 1–10 scale while retaining stored appearance until a control is adjusted.
- Set both absent frame-generation startup defaults off in the compiled AIO. Explicit saved settings and the normal app update/settings-preservation flow retain precedence.

Press Home for graphics settings. Textures remain required; Graphical Effects
is optional and experimental. Numpad * switches TextureQuest textures and effects
together. Native bloom remains the player's choice. Keep native lighting and
GPU shadows enabled when using renderer features that depend on those inputs.

Texture data remains 1.3.0. Only generated texture channels are distributed;
the installed game supplies untouched data. No game PAK replacement is used.

These are fix attempts for Tenri/Sichii's continued review, published before
local installation. No 1.13.6 in-world result or performance gain is claimed;
controlled tests have limited coverage. See [the collaborator handoff](https://github.com/Maergoth/TextureQuest/blob/v1.13.6/docs/GRAPHICS-1.13.6-HANDOFF.md)
for the exact behavior and remaining visual issues.

Thanks to Sichii for EQ2 Graphics and permission to include it. Third-party
components retain their respective licences and notices; runtime profile IDs
and retained private labels remain compatibility/history tokens.
