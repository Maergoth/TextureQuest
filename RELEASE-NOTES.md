# TextureQuest 1.13.13 — Develop

**What's new** now shows the installer's bundled changelog, labeled with the
installed app version. GitHub and the app use the same canonical
[CHANGELOG.md](CHANGELOG.md), including the backfilled 1.13.10–1.13.12 history.
Installer builds require an entry for their current app version.

This is an app-only Develop update. All native binaries remain at 1.13.12 and
texture data stays at 1.3.0. A Develop source push does not publish a new binary
release or change public main/latest.

The retained 1.13.12 renderer uses stable inferred particle-source identities
and a separate time-filtered light position while preserving current native
energy and dark pulses. It retains cached-light service-gap handling, the
optional dedicated particle character-shadow pool, and verified base-material
normal replay from earlier development builds. These are inherited fixes,
not new native changes in 1.13.13.

The existing shadow movement threshold, native-culling limits and source
association ambiguities remain. Controlled CPU/hardware cases and package
checks do not prove every model, shadow, offscreen effect or game FPS. See the
[graphics handoff](docs/GRAPHICS-1.13.12-HANDOFF.md) for the detailed scope and
[developer workflow](docs/CHANGELOG-WORKFLOW.md) for future changelog entries.
