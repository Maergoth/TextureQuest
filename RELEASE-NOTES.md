TextureQuest 1.13.9 — private Develop/local candidate

- Raise the particle-light ceiling to 128, intensity to 128× and radius control to 16× / 128 world units.
- Admit bounded independent particle draws: an oversized or unsupported draw no longer discards later fitting draws. Scene/epoch/event/map failures still fail closed; excluded-only observations do not refresh old energy.
- Correct WORLD shadow caster gathering and particle shadow lookup. Moving character shadows remain limited to the nearest 16 eligible lights, not all 128 particles.

The capture remains bounded to 512 draws / 32,768 triangles and can be partial;
draw order and native culling can affect source coverage. The exact production
CPU checks and one focused actual-D3D11 run passed. No historical alpha/full-suite
replay or new test trees were added. These checks do not prove all game coverage,
appearance or FPS; Sun movement flicker remains unresolved.

Saved controls and texture data 1.3.0 remain. Provider/AIO/NVIDIA and the other
seven native payloads retain their installed combined1.13.8 bytes; only the
renderer changes. The old1.13.8 normal update is consumed. This is a fresh private
package for local use and Develop review, with no public tag/release or main
update. The generator remains paused.
