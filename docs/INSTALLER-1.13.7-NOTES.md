# TextureQuest 1.13.7

App and hook updates now reuse an unchanged committed texture preparation instead of repeatedly reading and hashing every loose DDS file. The saved receipt, release/recipe identity, object roster, and texture map are still checked. Progress messages describe these metadata checks accurately.

The same texture version with changed preparation metadata requests an explicit Verify / Update repair. A missing preparation, different texture version, or legacy DDS preparation retains full verification. Explicit Verify / Update still checks texture bytes; the runtime's per-object SHA checks are unchanged. Cached admission is not a fresh integrity check of every DDS file.

Validation: 47 focused tests passed, including the actual setup, map activation, and cleanup stages with DDS stat/open access forbidden. The normal frozen 1.13.7 app passed its isolated UI smoke test. One source-level read-only measurement of an existing 23,875-object preparation completed receipt plus metadata/map admission in about 59 ms with no per-DDS path access. This timing covers that API check, not the entire update or rendering performance.

All eight native graphics payloads and the runtime descriptor are identical to 1.13.6. Texture data remains 1.3.0; its manifest and existing release bytes are unchanged. The existing Sun movement, particle visuals, and partial material-normal limitations remain as described in the [graphics handoff](GRAPHICS-1.13.6-HANDOFF.md).
