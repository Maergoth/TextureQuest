# EQ2 Graphics

A ReShade add-on for EverQuest II (Direct3D 11) that adds shadows, light and filtering the game does not have. Every
feature has a switch and settings in the add-on's panel in the ReShade overlay.

![The add-on's settings in the ReShade overlay](images/settings.png)

The game builds a light buffer each frame: the sun and ambient light first, then each lamp, which it adds by redrawing
the meshes near that lamp. Most features work by watching and changing those draws.

## Point light shadows

Lamps, torches and braziers cast soft shadows from the world and from moving characters.

Omnidirectional shadow mapping: each lamp gets a cube shadow map. Meshes seen in lamp passes and on screen are cached
with their world positions, so a lamp's map is rendered once from the cached meshes and kept, with a second map redrawn
every frame for moving meshes such as characters. Lamp passes are drawn with replacement pixel shaders that compute the
game's light and take off the shadow. Shadows are filtered with percentage-closer soft shadows (PCSS): a blocker search
finds how far the surface is behind what shadows it, and the filter widens with that distance over a Poisson disc, so
shadows are sharp at contact and soften further away.

## Inverse-square lamp light

Lamp light fades the way real light does, bright near the lamp with a soft tail, instead of the game's flat disc.

The game fades a lamp with a texture projected over its reach. The replacement lamp shaders work out each pixel's
distance from the lamp instead and use windowed inverse-square falloff: inverse square, multiplied by a window that
brings it smoothly to zero at the lamp's reach.

## Steady lamp light on characters

Characters and other moving objects stay lit by every lamp that reaches them, instead of the light switching on and
off as they move or the camera turns.

The game picks which lamps to draw a moving mesh for on the CPU, and leaves out lamps whose reach covers the mesh; it
also stops drawing a lamp once the lamp itself is off screen. The add-on keeps one draw of each moving mesh from the
frame, with its textures and constants, and once the lamps are done draws it again for every lamp in reach that the
game left out, all in one pass, with the same light and shadow as the replacement lamp shaders. Lamps are remembered
from earlier frames while the camera is near them, so a lamp off screen still lights the characters in view. It runs
with point light shadows and the replacement lamp shaders on.

## Emissive lights

Emissive surfaces such as lava light the area around them, with soft shadows.

The game only paints a faint glow around emissive meshes. The add-on treats emissive meshes as area lights: each is
read back once and split into cells on a grid fixed to the world. Cells that together cover every emissive mesh near
the camera, smaller nearer it, become up to 64 point lights, each lighting as a small glowing disc of the cell's area,
so the whole surface's light reaches the scene. They are drawn into the light buffer from depth (deferred, screen-space
lighting) with cube map shadows, softened by percentage-closer filtering (PCF) over a Poisson disc, and the game's
painted glow is dimmed where they reach. Each light also adds a soft, unshadowed bounce, nearly even across a cave's
width, standing in for light bouncing between the surfaces around it, so walls away from the emissive surface and the
backs of rocks are not left dark. Bright light eases off toward the light buffer's limit instead of clipping. The light
is found at half resolution, or 1440 lines if that is less, and upsampled by depth and normal.

## Soft sun shadows

Sun and moon shadow edges are softened, and lamp light is no longer darkened by them.

The game shadows the sun with cascaded shadow maps, darkening the finished image once per cascade. The add-on gives
those passes only the light from before the lamps, so lamp light fills sun shadows, and draws them with PCSS shaders.
Above 1440 lines the soft shadow is found at half resolution and brought back with depth-aware upsampling.

## Ambient occlusion

Corners and creases are darkened.

Screen-space ambient occlusion of the kind MXAO uses in its bitmask mode: horizon-based occlusion with a visibility
bitmask. Each pixel
takes 2 slices through the view direction with 8 depth samples to each side. Every sample is a slab of finite
thickness, and the angles it covers above the surface are set in a 32-bit mask whose sectors are spread evenly over
the angle from the normal rather than cosine-weighted. Unlike plain horizon-based occlusion (HBAO, GTAO), thin objects
then occlude only what they cover instead of everything below their top edge.

It runs at half resolution, half of the pixels each frame in a checkerboard, then a guided filter smooths it along
surfaces while keeping it apart at depth edges. It is taken off the light from before the lamps only, so lamp light is
not darkened by it.

## Anisotropic filtering

Textures stay sharp at shallow angles, at a chosen level up to 16x.

When the game binds a trilinear sampler, the add-on swaps in a copy with anisotropic filtering at the chosen level.

## Smooth interface text

Scaled interface text is smoothed instead of pixel-doubled.

When the game binds a point-filtered sampler, the add-on swaps in a copy with bilinear filtering.

## Frame capture

Writes a log of one frame's draws, state and images, for working on the add-on.

## Install

Install ReShade with full add-on support for `EverQuest2.exe`, then copy `EQ2Graphics.addon64` into the game folder
beside it.

## Build

Run `build.cmd` with the Visual Studio C++ build tools installed. It builds `out\EQ2Graphics.addon64` against the
ReShade and ImGui headers in `deps`.
