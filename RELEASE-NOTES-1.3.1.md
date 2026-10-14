TextureQuest app 1.3.1 makes normal direct-archive installation substantially
lighter: one copy with a streamed checksum, native game/patcher process checks,
and no resource decompression or repeated destination/stage/status hashes.
Downloads still match GitHub's archive identities. Recorded unchanged installed
files are skipped by file identity and modification metadata; Verify remains an
explicit full-checksum action. Interrupted copies keep their journal for retry.

Texture contents remain 1.2.0 and reuse the exact direct VPK assets from 1.3.0.
The same app checks installer and texture updates separately and keeps one ready
archive cache. Installation remains explicit and sideload remains disabled.
