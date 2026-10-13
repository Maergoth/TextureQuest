"""Discover a published release, then verify its manifest before any download.

GitHub's HTTPS release metadata supplies the asset checksum.
This establishes consistency with the repository owner's published release,
not a separate developer signature. No executable updates or game writes.
"""
from __future__ import annotations

from pathlib import Path
import re
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from texture_swapper import core
import texturequest_release as release
from texturequest_version import APP_VERSION

try:
    from texturequest_build_pin import APP_VERSION
except ImportError:
    pass

LATEST_API = "https://api.github.com/repos/Maergoth/TextureQuest/releases/latest"
REPOSITORY = "https://github.com/Maergoth/TextureQuest"
MANIFEST_NAME = "release-manifest.json"
HEADERS = {"User-Agent": "TextureQuest/" + APP_VERSION, "Accept": "application/vnd.github+json",
           "Accept-Encoding": "identity", "X-GitHub-Api-Version": "2022-11-28"}


def version_number(value):
    if not isinstance(value, str) or not re.fullmatch(r"(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})", value):
        raise ValueError("Unsupported TextureQuest version")
    return tuple(map(int, value.split(".")))


def _read(url, maximum, opener, pause):
    release.check_pause(pause)
    with opener(Request(url, headers=HEADERS), timeout=30) as response:
        final = urlsplit(response.geturl())
        if (response.getcode() != 200 or final.scheme != "https" or final.username or final.password
                or final.port not in (None, 443) or final.hostname not in
                {"api.github.com", "github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}):
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


def fetch_latest(state_dir=None, minimum_version="1.0.0", opener=urlopen, pause=None, current_manifest=None):
    """Fail closed on draft/prerelease, downgrade, foreign URLs or changed bytes."""
    from texturequest_version_check import _metadata, _version_asset, _pack, texture_version, pack_identity
    metadata = _metadata(opener, pause)
    if current_manifest is None and version_number(metadata["tag_name"][1:]) < version_number(minimum_version):
        raise ValueError("Latest release is older than the saved version")
    pack = _pack(metadata, _version_asset(metadata, opener, pause), opener, pause)
    if current_manifest is not None:
        current_version = texture_version(current_manifest)
        if version_number(pack["version"]) < version_number(current_version):
            raise ValueError("Latest textures are older than the saved version")
        if pack["version"] == current_version and pack["identity"] != pack_identity(current_manifest):
            raise ValueError("Published texture bytes changed without a texture version change")
    save_receipt(state_dir, metadata, pack)
    return pack["manifest"]


def save_receipt(state_dir, metadata, pack):
    receipt = {"schema": "texturequest-latest-check-v1", "checked_at": core._now(),
               "version": pack["manifest"]["version"], "texture_version": pack["version"],
               "release_id": metadata.get("id"), "release_url": metadata["html_url"],
               "manifest_url": pack["manifest_url"], "manifest_sha256": pack["manifest_sha256"], "manifest": pack["manifest"]}
    if state_dir is not None:
        state_dir = core._plain(Path(state_dir))
        core._save(core._plain(state_dir / "latest-release.json", state_dir), receipt)
