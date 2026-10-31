TextureQuest 1.7.1 fixes Graphics hook installation failing with:

> output must be inside the loader component

The installer now puts offline preparation in the bundled tool's permitted component folder. The graphics cache remains separate, with receipts linking the two. Existing downloads and failed preparation history are preserved.

Update the app, then click **Install graphics hook** again. No texture redownload is needed. Keep EQ2 closed during installation.

The failure was reproduced against 1.7.0. A fresh-install test now runs the actual bundled preparation, READY verification, bindings and graphics catalog path using tiny synthetic archives. Graphics binaries, texture pack 1.2.3 and released texture URLs are unchanged.
