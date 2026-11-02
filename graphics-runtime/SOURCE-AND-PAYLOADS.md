# Source versus runtime payloads

This source bundle contains no executable runtime payload. After the normal
1.13.5 local install is verified, copy the exact final package002 RUNTIME.json
here into the app checkout and preserve the corresponding notices. Do not
publish this staging note in place of that descriptor and do not reuse the old
tracked schema-v6 descriptor or its obsolete two DLLs.

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
