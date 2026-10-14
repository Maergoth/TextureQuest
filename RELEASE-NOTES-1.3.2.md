TextureQuest app1.3.2 adds overall download and installation progress: cumulative percentage, bytes and PAK count across the complete pack, with current-file status retained. The bar no longer resets between PAKs or spins indefinitely during installation.

This release includes the fast1.3.1 path: downloaded files are verified from their incoming byte stream, without decompressing each texture or repeatedly scanning each finished archive between downloads. Installation hashes bytes as it copies and uses inexpensive native game/launcher checks. Matching prepared downloads and unchanged installed files are reused. Explicit Verify remains available.

Texture contents remain1.2.0: the same310 full VPKs,23,282 replacements,74 selected4K maps and20 prior repairs. Existing1.3.0 VPK downloads are reused; no texture was rebuilt or uploaded again.

Close the app before upgrading. Extract the new installer and retain your existing data folder alongside TextureQuest.exe. Downloads/preparation never install game files automatically; click Install after patching and closing EQ2 and its launcher. Sideload remains unavailable.
