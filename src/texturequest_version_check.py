"""Separate app/texture discovery from GitHub's checksum-bearing asset metadata.

No executable is downloaded or replaced. A failed channel never supplies a
manifest or an "up to date" result. Release tags identify publication envelopes;
texture versions and byte identities identify the actual texture data.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
from urllib.error import HTTPError, URLError
from urllib.request import urlopen

from texture_swapper import core
import texturequest_release as release
import texturequest_latest as latest

VERSION_ASSET = "texturequest-version.json"
MAX_VERSION_BYTES = 16 * 1024
CHECK_SCHEMA = "texturequest-update-check-v1"


def texture_version(manifest):
    version = (manifest.get("texture_version") if manifest.get("schema") in ("texturequest-release-v3", "texturequest-release-v4")
               else manifest.get("update", {}).get("version", manifest.get("version")))
    latest.version_number(version)
    return version


def pack_identity(value):
    """Ignore envelope versions and archive display metadata, never byte identity."""
    if value.get('schema') == 'texturequest-release-v4':
        return core._digest(value['channel_source'])
    rows = value.get("archives", [])
    if not isinstance(rows, list) or not rows:
        return None
    identities = []
    seen = set()
    for row in rows:
        name = core._name(row.get("name"))
        if name.casefold() in seen or type(row.get("bytes")) is not int or row["bytes"] <= 0:
            raise ValueError("Invalid texture identity inventory")
        if not isinstance(row.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", row["sha256"]):
            raise ValueError("Invalid texture identity checksum")
        seen.add(name.casefold())
        identities.append((name, row["bytes"], row["sha256"]))
    return core._digest(sorted(identities))


def _metadata(opener, pause):
    value = core._json(latest._read(latest.LATEST_API, core.MAX_MANIFEST_BYTES, opener, pause))
    if not isinstance(value, dict):
        raise ValueError("Invalid GitHub release metadata")
    tag = value.get("tag_name", "")
    version = tag[1:] if isinstance(tag, str) and tag.startswith("v") else ""
    latest.version_number(version)
    if (value.get("draft") is not False or value.get("prerelease") is not False
            or not value.get("published_at") or value.get("html_url") != latest.REPOSITORY + "/releases/tag/" + tag
            or not isinstance(value.get("assets"), list)):
        raise ValueError("Latest release is unpublished or unexpected")
    return value


def _asset(metadata, name, maximum):
    rows = [row for row in metadata["assets"] if isinstance(row, dict) and row.get("name") == name]
    if len(rows) != 1:
        raise ValueError("Release has no unique " + name + " asset")
    row = rows[0]
    url = latest.REPOSITORY + "/releases/download/" + metadata["tag_name"] + "/" + name
    if (row.get("state") != "uploaded" or row.get("browser_download_url") != url
            or type(row.get("size")) is not int or not 0 < row["size"] <= maximum
            or not isinstance(row.get("digest"), str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", row["digest"])):
        raise ValueError("Invalid published " + name + " identity")
    return row


def _json_asset(metadata, name, maximum, opener, pause):
    row = _asset(metadata, name, maximum)
    raw = latest._read(row["browser_download_url"], row["size"], opener, pause)
    if len(raw) != row["size"] or hashlib.sha256(raw).hexdigest() != row["digest"][7:]:
        raise ValueError(name + " failed checksum verification")
    value = core._json(raw)
    if not isinstance(value, dict):
        raise ValueError("Invalid " + name)
    return value, row


def _version_asset(metadata, opener, pause):
    if not any(isinstance(row, dict) and row.get("name") == VERSION_ASSET for row in metadata["assets"]):
        return None
    value, _ = _json_asset(metadata, VERSION_ASSET, MAX_VERSION_BYTES, opener, pause)
    keys = {"schema", "app_version", "texture_version", "manifest_version", "manifest_sha256", "installer_name"}
    if set(value) != keys or value["schema"] != "texturequest-version-v1":
        raise ValueError("Unsupported application/texture version metadata")
    for name in ("app_version", "texture_version", "manifest_version"):
        latest.version_number(value[name])
    if (value["installer_name"] != f"TextureQuest-Installer-{value['app_version']}-windows-x64.zip"
            or not isinstance(value["manifest_sha256"], str)
            or not re.fullmatch(r"[0-9a-f]{64}", value["manifest_sha256"])):
        raise ValueError("Invalid application/texture version metadata")
    return value


def _installer(metadata, explicit):
    if explicit:
        version, name = explicit["app_version"], explicit["installer_name"]
    else:
        # Compatibility for already published releases: identify the actual app
        # from its asset, never assume a tag is the app or texture data version.
        rows = [row for row in metadata["assets"] if isinstance(row, dict)
                and re.fullmatch(r"TextureQuest-Installer-[0-9]+\.[0-9]+\.[0-9]+-windows-x64\.zip", str(row.get("name", "")))]
        if len(rows) != 1:
            raise ValueError("Release has no unique Windows installer")
        name = rows[0]["name"]
        version = name[len("TextureQuest-Installer-"):-len("-windows-x64.zip")]
    latest.version_number(version)
    row = _asset(metadata, name, 512 * 1024**2)
    return {"status": "verified", "version": version, "download_url": row["browser_download_url"],
            "release_url": metadata["html_url"], "bytes": row["size"], "sha256": row["digest"][7:]}


def _pack(metadata, explicit, opener, pause):
    value, row = _json_asset(metadata, latest.MANIFEST_NAME, core.MAX_MANIFEST_BYTES, opener, pause)
    manifest = release.validate_manifest(value, pinned=True)
    if explicit:
        if (manifest["version"] != explicit["manifest_version"] or texture_version(manifest) != explicit["texture_version"]
                or row["digest"][7:] != explicit["manifest_sha256"]):
            raise ValueError("Manifest differs from the explicit texture version pin")
    elif manifest["version"] != metadata["tag_name"][1:]:
        raise ValueError("Manifest version differs from its GitHub release")
    return {"status": "verified", "version": texture_version(manifest), "manifest": manifest,
            "identity": pack_identity(manifest), "release_url": metadata["html_url"],
            "manifest_url": row["browser_download_url"], "manifest_sha256": row["digest"][7:]}


def error_message(error):
    if isinstance(error, HTTPError) and error.code in (403, 429):
        return "GitHub limited this request. Try again later; saved files are unchanged."
    if isinstance(error, (URLError, TimeoutError, ConnectionError, OSError)):
        return "Could not reach GitHub. Try again when connected; saved files are unchanged."
    return str(error) or "The published version could not be verified."


def installer_page(result):
    """Only the fixed project's stable release page is an upgrade action."""
    url = result.get("release_url", "")
    prefix = latest.REPOSITORY + "/releases/tag/v"
    if result.get("status") != "verified" or not isinstance(url, str) or not url.startswith(prefix):
        raise ValueError("No verified official application release page")
    latest.version_number(url[len(prefix):])
    return url


def saved_manifest(state_dir, bundled):
    """Recover a locally saved selection, not a successful live update check.

    Legacy receipts lack a record checksum. Their bounded, compatible manifest
    and official links may establish a version floor, but installation still
    requires a fresh GitHub check and complete byte verification.
    """
    path = core._plain(Path(state_dir) / "latest-release.json", state_dir)
    if not path.exists():
        return bundled
    if path.stat().st_size > core.MAX_MANIFEST_BYTES:
        raise ValueError("Saved release metadata is too large")
    value = core._json(path.read_bytes())
    if not isinstance(value, dict) or value.get("schema") != "texturequest-latest-check-v1":
        raise ValueError("Saved release metadata is invalid")
    if "record_sha256" in value:
        value = core._load(path)
    manifest = release.validate_manifest(value.get("manifest", {}), pinned=True)
    url = value.get("release_url", "")
    prefix = latest.REPOSITORY + "/releases/tag/v"
    if not isinstance(url, str) or not url.startswith(prefix):
        raise ValueError("Saved release URL is invalid")
    tag_version = url[len(prefix):]
    latest.version_number(tag_version)
    if (value.get("version") != manifest["version"]
            or value.get("manifest_url") != latest.REPOSITORY + "/releases/download/v" + tag_version + "/" + latest.MANIFEST_NAME
            or not re.fullmatch(r"[a-f0-9]{64}", str(value.get("manifest_sha256", "")))
            or ("texture_version" in value and value["texture_version"] != texture_version(manifest))):
        raise ValueError("Saved release identity is invalid")
    comparison = latest.version_number(texture_version(manifest)), latest.version_number(texture_version(bundled))
    if comparison[0] < comparison[1]:
        return bundled
    if comparison[0] == comparison[1] and pack_identity(manifest) != pack_identity(bundled):
        raise ValueError("Saved texture bytes differ without a version change")
    if bundled.get("schema") == "texturequest-release-v3" and manifest.get("schema") != "texturequest-release-v3":
        return bundled  # Reuse legacy VPK bytes through migration, never the legacy ZIP workflow.
    return manifest


def check_updates(current_manifest, app_version, state_dir=None, opener=urlopen, pause=None):
    """Return independent channel results; retain a saved release on any failure."""
    latest.version_number(app_version)
    current_version = texture_version(current_manifest)
    current_identity = pack_identity(current_manifest)
    result = {"schema": CHECK_SCHEMA, "checked_at": core._now()}
    try:
        metadata = _metadata(opener, pause)
        explicit = _version_asset(metadata, opener, pause)
    except release.Paused:
        raise
    except Exception as error:
        failed = {"status": "unavailable", "message": error_message(error)}
        return dict(result, installer=dict(failed), pack=dict(failed))
    for key, action in (("installer", lambda: _installer(metadata, explicit)),
                        ("pack", lambda: _pack(metadata, explicit, opener, pause))):
        try:
            result[key] = action()
        except release.Paused:
            raise
        except Exception as error:
            result[key] = {"status": "unavailable", "message": error_message(error)}
    pack = result["pack"]
    if pack["status"] == "verified":
        if current_manifest.get('schema') == 'texturequest-release-v4' and pack['manifest'].get('schema') != 'texturequest-release-v4':
            result['pack'] = {'status':'unavailable','message':'This app requires generated-channel texture data. Saved files are kept.'}
        elif current_manifest.get("schema") == "texturequest-release-v3" and pack["manifest"].get("schema") != "texturequest-release-v3":
            result["pack"] = {"status": "unavailable", "message": "This app requires direct VPK downloads. The latest release is using an older format."}
        elif latest.version_number(pack["version"]) < latest.version_number(current_version):
            result["pack"] = {"status": "unavailable", "message": "GitHub returned older textures. Saved version is kept."}
        elif pack["version"] == current_version and pack["identity"] != current_identity:
            result["pack"] = {"status": "unavailable", "message": "Published texture bytes changed without a texture version change. Saved version is kept."}
        else:
            pack["update_available"] = pack["version"] != current_version
            if state_dir is not None:
                try:
                    latest.save_receipt(state_dir, metadata, pack)
                except (OSError, ValueError) as error:
                    result["pack"] = {"status": "unavailable", "message": "Could not save the verified texture release. " + str(error)}
    app = result["installer"]
    if app["status"] == "verified":
        comparison = latest.version_number(app["version"]) > latest.version_number(app_version)
        app["update_available"] = comparison
        app["local_newer"] = latest.version_number(app["version"]) < latest.version_number(app_version)
    return result
