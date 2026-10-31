# Individual DDS installation

The installer uses the existing **TextureQuest-Textures** release. It downloads
`texture-source.json`, its SHA-256-pinned `texture-index.json`, and required ZIP
chunks. Chunks are transport only: the installed payload is individual DDS files.
No replacement VPK is downloaded, rebuilt, or installed.

The source/index retain every archive/member identity, category, map type, source
layout/fingerprint and target mip/tail identity. Named aliases can share one DDS
object without losing their categories.

## Shared originals

One identical stock layout/fingerprint gets one upscale:

1. Prefer the known correction with the latest correction revision.
2. Otherwise select the target outcome used by the most named copies.
3. Break ties deterministically using layout/fingerprint and DDS identity.

`src/shared-texture-choices.json` records the current result and correction
priorities. New groups are selected automatically. There is no appearance review
or per-texture approval step. The current 1.2.4 data has 208 conflicting groups
covering 486 named files; all receive a shared choice. The four native-AI
corrections in 1.2.4 and six Ratonga corrections in 1.2.1 have priority over ordinary
generations. Existing variants and masters are preserved; nothing is regenerated.

For a later correction, record its named ID and increasing revision:

```powershell
python tools/select_shared_textures.py texture-index.json choices.json --correction <named-id>:125
```

Existing correction priorities are carried forward. Texture releases must remain
cumulative so later exports never revert earlier selected repairs.
Future published index rows can carry a positive, increasing `correction_revision`;
the installer then prefers the later correction without requiring an app update.
Do not infer correction order from download/extraction file timestamps.

## Download and activation

- GitHub asset metadata supplies the manifest checksum; the manifest pins the
  index and ZIPs. Downloads reuse bounded range requests and checkpoint journals.
- Extraction verifies exact ZIP membership/type and full DDS SHA-256, layout,
  mip fingerprint and opaque tail. Paths are `dds/<sha256>.dds`. Existing unknown
  or corrupt objects are preserved and refused, never overwritten.
- Only selected objects are stored. One ZIP is extracted at a time and removed
  after verification. Completed DDS files and interrupted checkpoints survive.
- The installer builds the existing XXH3 lookup by reading selected stock members
  from the client's PAKs with writes denied. Exact source DDS SHA-256, layout and
  TQABUP01 fingerprint must match. Changed or missing sources remain pending;
  filename equality is not remapping authority.
- Objects and finished maps are immutable. The ready receipt commits only after
  all selected DDS and map checks. An interrupted update leaves the old map and
  its referenced files intact. Activation replaces only the map pointer with
  EQ2 closed; an installed DDS hook uses it on its next launch.

The only game-folder writes are explicit hook Install/Uninstall. Existing Replace
users restore stock with the official patcher once. Old PAK downloads are kept.

## Runtime contract

Stock ReShade 6.8.0 and PR #1's graphics callbacks remain. A DDS reader replaces
the replacement-VPK reader. It denies writes during the read, closes its handle,
verifies the full file SHA-256 and dimensions/mips, and supplies the existing
creation hook. Headers, channel representation, mip bytes and tails are unchanged.

`TQMAP002` retains the sorted XXH3-128 lookup and 88-byte rows. The file table
adds full-file size, string offset and SHA-256. Paths must equal
`dds/<digest>.dds`; arbitrary paths and legacy PAK maps cannot supply replacements.
Stock PAK reads remain for Original comparison only.

The product has two states: **Original / Upscaled**. The private three-way hall
test is separate and untouched. Full-pack EQ2 validation is required before
publishing a new installer; this PR does not install or launch a game.
