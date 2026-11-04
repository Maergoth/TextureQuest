# Renderer 1.13.11 local development build

This local build fixes particle allocation-service gaps that erased cached
light/map identities. Existing committed lights still fade and expire, while
a valid zero budget, explicit Off and scene resets retain their meaning.

Particle contributions are grouped into4-unit world regions before retention,
conserving RGB energy and weighted centroids. The203439 selected-light capture
simulation reduces128lights to15groups; this is not a measured FPS promise.
The native/dedicated shadow layouts and light HLSL remain unchanged from1.13.10.

Verified base PS ebfb1c1d07ad558d with VS92f68ad8f88a96b3 or4780c178ee4ee3ea
uses the native weighted T0 normal recipe. Admitted opaque draws now use that
existing material-normal shader path, preserving smooth basis interpolation.
Unknown, alpha-tested or unsupported draws retain the geometric fallback.

Final strict/W4/WX native build passed. Focused production CPU checks passed
for allocation/selection/slots(70) and grouping/readback/cache(54). One new
actual hardware-D3D11 normal case passed native base floor orientation, smooth
interpolation, weighting/refusal guards and native state/data preservation.
No previous128-map/alpha/full suite was replayed. RELEASE-SOURCE.json records
the compiled image and30root native inputs. Existing dependency permission and
licenses remain unchanged; captured game shaders/native outputs are not added
to source control. See docs/GRAPHICS-1.13.11-HANDOFF.md.
