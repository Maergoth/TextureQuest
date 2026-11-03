# Source versus runtime payloads

This source bundle contains no executable runtime payload. RUNTIME.json pins
the current 1.13.8 candidate: standalone renderer020 and the conditional handled-F10
provider, with the other six native payloads unchanged from1.13.6/1.13.7.
RELEASE-1138-PROVENANCE.json records the source/build and controlled-evidence
joins. Older release provenance and notices remain historical records. The standalone renderer020
payload (0d092e78…) differs from the separate local PR4 integration (554bb0ab…);
this branch contains no PR4 renderer code or combined binary.

Build the installer with an explicit --graphics-runtime-dir pointing to the
verified complete release runtime. build_installer.py validates every selected
payload hash; a descriptor-only Git checkout is intentionally not a complete
runtime directory. Distribute DLL/addon64 payloads in release assets, not Git.
The v8/profile IDs, including historical 'private' words, are compatibility
tokens accepted by the implementation. They are not license grants or evidence
that old binaries are current. Exact file hashes determine the payload identity.

The archived PRIVATE notices describe their original test scopes. The maintainer
has now explicitly confirmed Sichii redistribution permission. That supersedes
the earlier 'obtain permission' readiness condition, not third-party licenses.
