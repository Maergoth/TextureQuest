"""Discover a published release, then verify its manifest before any download.

GitHub's HTTPS release metadata supplies the asset checksum.
This establishes consistency with the repository owner's published release,
not a separate developer signature. No executable updates or game writes.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from texture_swapper import core
import texturequest_release as release

LATEST_API = "https://api.github.com/repos/Maergoth/TextureQuest/releases/latest"
REPOSITORY = "https://github.com/Maergoth/TextureQuest"
MANIFEST_NAME = "release-manifest.json"
HEADERS = {"User-Agent": "TextureQuest/1.1.1", "Accept": "application/vnd.github+json",
           "Accept-Encoding": "identity", "X-GitHub-Api-Version": "2022-11-28"}


def version_number(value):
    if not isinstance(value, str) or not re.fullmatch(r"(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})", value):
        raise ValueError("Unsupported TextureQuest version")
    return tuple(map(int, value.split(".")))


def _read(url, maximum, opener, pause):
    release.check_pause(pause)
    with opener(Request(url, headers=HEADERS), timeout=30) as response:
        if response.getcode() != 200 or urlsplit(response.geturl()).scheme != "https":
            raise ValueError("Release server returned an invalid response")
        length = response.headers.get("Content-Length")
        if length is not None and (int(length) < 1 or int(length) > maximum):
            raise ValueError("Release metadata exceeds the supported size")
        data = bytearray()
        while chunk := response.read(min(64 * 1024, maximum + 1 - len(data))):
            release.check_pause(pause)
            data.extend(chunk)
            if len(data) > maximum:
                raise ValueError("Release metadata exceeds the supported size")
        return bytes(data)


def fetch_latest(state_dir=None, minimum_version="1.0.0", opener=urlopen, pause=None):
    """Fail closed on draft/prerelease, downgrade, foreign URLs or changed bytes."""
    metadata = core._json(_read(LATEST_API, core.MAX_MANIFEST_BYTES, opener, pause))
    tag = metadata.get("tag_name", "")
    version = tag[1:] if isinstance(tag, str) and tag.startswith("v") else ""
    if (metadata.get("draft") is not False or metadata.get("prerelease") is not False
            or not metadata.get("published_at") or version_number(version) < version_number(minimum_version)
            or metadata.get("html_url") != REPOSITORY + "/releases/tag/" + tag):
        raise ValueError("Latest release is unpublished, unexpected or older than the saved version")
    expected_url = REPOSITORY + "/releases/download/" + tag + "/" + MANIFEST_NAME
    assets = [row for row in metadata.get("assets", []) if row.get("name") == MANIFEST_NAME]
    if len(assets) != 1:
        raise ValueError("Latest release has no unique compatible manifest. Download its new installer from GitHub.")
    asset = assets[0]
    digest = asset.get("digest", "")
    if (asset.get("state") != "uploaded" or asset.get("browser_download_url") != expected_url
            or type(asset.get("size")) is not int or not 0 < asset["size"] <= core.MAX_MANIFEST_BYTES
            or not isinstance(digest, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest)):
        raise ValueError("Latest manifest metadata or checksum is invalid")
    raw = _read(expected_url, asset["size"], opener, pause)
    if len(raw) != asset["size"] or hashlib.sha256(raw).hexdigest() != digest[7:]:
        raise ValueError("Latest release manifest failed checksum verification")
    manifest = release.validate_manifest(core._json(raw), pinned=True)
    if manifest["version"] != version:
        raise ValueError("Manifest version differs from its GitHub release")
    receipt = {"schema": "texturequest-latest-check-v1", "checked_at": core._now(), "version": version,
               "release_id": metadata.get("id"), "release_url": metadata["html_url"],
               "manifest_url": expected_url, "manifest_sha256": digest[7:], "manifest": manifest}
    if state_dir is not None:
        state_dir = core._plain(Path(state_dir))
        core._atomic(core._plain(state_dir / "latest-release.json", state_dir), receipt)
    return manifest
