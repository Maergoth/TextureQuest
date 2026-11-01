# Channel downloads and the local DDS cache

TextureQuest 1.9.1 downloads channel-source.json, its pinned channel-index.json
and required channels-*.zip chunks from TextureQuest-Textures. Downloads contain
only generated/corrected RGB channels plus layout/lookup metadata. No original
alpha planes, original DDS headers/tails, whole DDS or game PAKs are downloaded.

Install graphics hook downloads, reconstructs, builds the map and installs the
hook automatically. Download / Resume can also prepare the cache separately.
About 17.7 GB downloads become about 27.8 GB of local reconstructed textures.
The player's existing EQ2 installation supplies protected original channels.

## Reconstruction and updates

- Exact original DDS SHA/layout/mip fingerprint, channel ownership, payload SHA,
  wraps and target DDS/mip/tail hashes must match. No downloaded-stock fallback.
- The CPU recipe matches the approved local test: padded float32 Lanczos4,
  explicit per-axis wraps, BOX mips and pinned DirectXTex. No AI model is needed.
- Four released RGB profiles are admitted. Alpha-only, scalar, joint and signed
  generations need separate ownership profiles before future publication.
- The 46 explicit new source-alpha references retain prior hashes as history;
  BC1 may requantize generated colour blocks where transparency requires it.
- Cache files and maps are immutable and verified before reuse. Interrupted
  downloads resume using checked prefixes; completed local DDS survive.
- Current source matching builds the lookup automatically from the local PAKs,
  read with writes denied. Changed or missing source groups remain original and
  are reported explicitly. No manual texture-to-model mapping is needed.
- An interrupted update keeps the old map and its referenced textures. Activation
  happens only with EQ2 closed; the hook loads it on the next normal launch.

Local mixed-origin DDS must never be re-uploaded. Public payloads remain separate
from this private cache. Categories and every named alias stay in the index.
Shared originals prefer known later corrections, then the most-used version with
a stable tie-break. All accepted body, stone and Ratonga corrections are retained.

## Graphics runtime

Stock full-add-on ReShade 6.8 and the reviewed graphics callbacks remain.
TQMAP002 maps sorted XXH3-128 original mip fingerprints to local DDS files.
Each local file is checked for SHA-256, size, layout and mips before use.
File paths are restricted to dds/<digest>.dds; arbitrary paths are refused.

The product has two states: Original / Upscaled. Numpad * toggles them and the
installer can change the shortcut. No background watcher or service. ReShade
loads the add-on on normal EQ2 launches, including after reboot.

Game PAKs stay unchanged. Existing Replace users should restore stock with the
official patcher once. Uninstall removes only owned, unchanged hook files and
keeps the local cache. Get app update preserves portable data.
