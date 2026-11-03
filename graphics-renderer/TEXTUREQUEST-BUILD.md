# Renderer025/026 local development build

Private1.13.10 adds dedicated moving-character maps for up to128 particle lights,
configured shadow strength/softness for emissive lighting, and a bounded observed
source cache (2 seconds full weight,1 second fade;256 entries). Native16x512 maps
and the80-byte glow-light ABI are preserved. Particle banks use256px faces and
up to192MiB plus buffers. All128 maps add caster work; game FPS is unmeasured.

Base-only draws no longer claim an unproved material normal: the captured floor
and wall use geometric depth fallback. Confirmed material/TBN normals remain.
Character-section seams are not yet visually verified.

RELEASE-SOURCE.json records the final29 root native inputs and compiled image.
Strict/W4/WX build and one focused actual hardware-D3D11 run passed: slot33 and
all128 maps, strength0/half/full, softness edge response, pose/epoch/off retirement,
and native-pool/state preservation. The cache passed84 focused checks. No old
alpha/full-suite replay. Native-culled sources never observed cannot be inferred.

The normal portable installer is built from this working branch with textures
unchanged at1.3.0 and seven other native payloads unchanged. GitHub is held per
the user's local-first instruction. See docs/GRAPHICS-1.13.10-HANDOFF.md.
