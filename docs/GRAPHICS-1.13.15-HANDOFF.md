# 1.13.15 public graphics release

Base Develop: 3ca6da05ea6bdf896bb548e09fef4ccf9fc0e293. This carries the
previous Develop particle grouping, retained-source, material-normal replay,
character-shadow pool and bounded source-core changes into a public package.
Texture data stays 1.3.0. No unfinished generation outputs are released.

The launcher visibly includes DLSS and says Home shows/hides settings. Fresh
profiles enable AO and16xAF. FG,NR,smoothtext,point/emissive and all Sun work
start off. Existing saved options/custom values remain; forced old renderer
preset rewriting is removed. The new default lease schema is v2 and retains
exact v1 restoration authority for previous owned settings.

Upscaling is retired at the managed menu, startup config, ABI mailbox ingress,
SR initialization/creation/evaluation and native extent selection. ABI sizes
and exports stay compatible. FG/NR-only recreation accepts a null SR handle.
The core no longer loads saved Reflex preferences, instantiates its driver
controller, submits mode requests or draws Reflex controls.

Quality mapping:
- Shadows Low10/10,Med50/50,Max512/128: globalmap/particlecharactershadow budgets.
- Lights Low16/16,Med64/64,Max512/128: emissive/particlebudgets.
- Native lamps take priority in the shared shadow pool; a lower shadow preset
  can constrain a higher light preset. Advanced retains custom numeric values.

Validation: all3 native images rebuilt. Core/renderer have0 warnings; renderer
uses W4/WX. AIO retains14 unchanged-source/MinHook warnings. CPU mailbox checks,
renderer preset/sun/work checks, current/default/old-lease restoration cases and
launcher caption/layout/changelog cases passed. Exact native source and image
pins are in each component's RELEASE-SOURCE/RELEASE-NATIVE-SOURCE metadata.
Normal installer ZIP/frozen UI verification is part of the final release gate.

Five other native images, including the later F10-compatible provider, remain
unchanged from local1.13.14. No new model/GPU/shader test run is required or
claimed for this controls/defaults-only delta. Prior controlled lighting checks
remain historical, not universal in-game appearance or FPS proof. Sun-direction
movement flicker is unresolved. Sources the game never submits, unsupported
shader paths and some self-occlusion remain limitations.

No generator control/source/configuration, scheduler, globalGPU or gamePAK
changes belong to this app-only public release. Local app/game installation is
separate from publishing the normal update package.
