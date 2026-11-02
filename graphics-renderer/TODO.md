# To do

## Point light shadows

- **Alpha-tested geometry casts solid shadows.** Foliage and fences cast their whole quads. Lamp passes never
  alpha-test; the main pass does, when b1 row c0.x is above 0.5, with the compare function in c0.z (D3D numbering) and
  the reference in c0.y. The tested alpha blends up to four texture layers (t0 to t3) with weights from b0, so the caster
  pass should replay the mesh's main-pass shaders, constants and textures with the position rows c3 to c6 replaced by
  the light's, rather than rebuild the alpha per shader. In one lamp capture, 50 of 184 lamp-lit meshes were
  alpha-tested this way.
- **Objects never seen on screen cast nothing.** Casters are cached from the lamp passes and from the light buffer's
  base light, which draws every opaque mesh on screen; an object that has not been on screen yet is unknown. The game's
  sun shadow maps did not help: in the outdoor capture checked they held only the player's character. A cached object is given to every lamp
  whose shadow range holds its origin, so a large mesh whose origin lies outside that range casts only once seen in the
  lamp's own pass. Cached objects near the camera are kept while out of view; one removed from the world is dropped once
  its spot is seen in view without it, and keeps casting until then. Ground cover (grass tufts, small shrubs) is
  drawn each frame as one batch of small double-sided quads, from a vertex buffer the CPU rewrites with world positions,
  alpha-tested at alpha above 0.5. It never appears in a lamp pass, so lamps do not light it and it casts nothing.
  Casting would mean copying that batch from the main pass each frame into the moving-object maps, with its alpha test.
- **Billboards count as moving.** They turn to face the camera, so they go into the per-frame moving-object maps, and
  their shadow is the quad as it faces the camera rather than as it would face the light.
- **Meshes shared by several moving characters.** A mesh drawn at more than one position in a frame is never marked
  moving by its position, so identical unanimated objects that move are cached as static objects at each place they
  stood, until eviction. Animated meshes, whose vertices the CPU rewrites, count as moving whatever their position, which
  covers most mobs; not yet checked with a group of identical mobs.
- **Lamp shadows in daylight.** Some outdoor fires are almost half as bright as the sun, so their shadows stay strong
  in daylight. Tune once a smaller outdoor light can be measured.
- **Lamp shaders not yet seen.** Lamp passes whose pixel shader has no replacement in `LAMP_SHADERS` still get an extra
  shadow draw. A frame capture where "extra lighting draws" is high shows the missing shader.
- **Emissive lights beyond the magma cave.** An emissive mesh is found by its glow pass, a draw that adds its vertex colour
  with no texture after the light buffer is finished; textured passes that add glow without the light buffer paint a
  faint glow onto nearby surfaces and are dimmed where emissive lights reach. Both rules come from the magma cave; other
  glowing things, such as crystals, may be drawn another way and need a capture each.
- **Emissive light strength.** The light from an emissive cell is its measured colour times its area over pi, at strength 1.
  Tune the default by eye.
- **Lamp falloff brightness.** Tune `FindFalloff`'s brightness (0.25) and hot-spot size (0.125) once seen in game.

## Sun shadows

- **Lights added before the lamps.** Between the base light and the lamps the game adds more lights to the light
  buffer, each a vertex light colour times bump shading. They are treated as base light, darkened by the sun shadow and
  ambient occlusion: leaving them all undarkened weakened sun shadows away from any lamp, so at least one of them
  carries sunlight. Leaving only the small lights undarkened would need the sunlight one told apart from the rest.

## Notes

- **Where the camera is.** Most of the game's vertex constants hold the camera's world position in row c16 (w is
  0.0001), with row c19 set to 10 100 1000 10000. Light passes are drawn relative to it. The frame's first draws, into
  small textures without a depth buffer, still hold the previous frame's camera, and water reflection draws hold a
  camera mirrored in the water; only draws into a screen-sized depth buffer are read.

- **Read the game's constants on the CPU.** The game writes its light-pass constants from the CPU into a dynamic
  buffer. A GPU copy of those constants per draw cost about a quarter of the GPU's time; reading them from the mapped
  buffer costs almost nothing.
- **GPU usage percentages mislead.** Compare power draw and clock speed (for example with `nvidia-smi`) when
  measuring the add-on's cost.
