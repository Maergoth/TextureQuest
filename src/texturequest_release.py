"""Pinned public release download and verification; never writes game files."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import zipfile
import zlib

from texture_swapper import core

CHUNK = 1024 * 1024
RELEASE = "https://github.com/Maergoth/TextureQuest/releases/download/v1.0.0/"
PIN_SHA = "9501071759d57c26175ee9245b3f5dfee4d4cee7f19a57c039289d09d2041caf"
PIN_BYTES = 27763745032


class Paused(Exception):
    pass


def check_pause(pause):
    if pause and pause():
        raise Paused("Download paused. Download / Resume will continue it.")


def hash_stream(stream, length=None, pause=None):
    digest = hashlib.sha256()
    remaining = length
    while remaining is None or remaining:
        check_pause(pause)
        chunk = stream.read(CHUNK if remaining is None else min(CHUNK, remaining))
        if not chunk:
            if remaining:
                raise ValueError("Truncated file while verifying")
            break
        digest.update(chunk)
        if remaining is not None:
            remaining -= len(chunk)
    return digest.hexdigest()


def validate_manifest(value, pinned=True):
    if value.get("schema") == "texturequest-release-v2":
        from texturequest_updates import validate_manifest as validate_update
        return validate_update(value, pinned=pinned)
    if value.get("schema") != "texturequest-release-v1" or value.get("version") != "1.0.0":
        raise ValueError("Unsupported TextureQuest release manifest")
    archive = value.get("archive", {})
    if archive.get("name") != "Texture-Pack.zip":
        raise ValueError("Unexpected output filename")
    if not isinstance(archive.get("bytes"), int) or archive["bytes"] <= 0:
        raise ValueError("Invalid archive size")
    if not re.fullmatch(r"[a-f0-9]{64}", str(archive.get("sha256", ""))):
        raise ValueError("Invalid archive checksum")
    if pinned and (archive["sha256"] != PIN_SHA or archive["bytes"] != PIN_BYTES):
        raise ValueError("This installer is pinned to the approved TextureQuest 1.0.0 ZIP")
    parts = value.get("parts", [])
    if not isinstance(parts, list) or not 1 <= len(parts) <= 100:
        raise ValueError("Invalid download parts")
    total = 0
    for index, part in enumerate(parts, 1):
        name = f"TextureQuest-1.0.0.zip.{index:03d}"
        if part.get("name") != name or part.get("url") != RELEASE + name:
            raise ValueError("Unexpected download filename or release URL")
        if not isinstance(part.get("bytes"), int) or not 0 < part["bytes"] < 2**31:
            raise ValueError("Invalid download part size")
        if not re.fullmatch(r"[a-f0-9]{64}", str(part.get("sha256", ""))):
            raise ValueError("Invalid download part checksum")
        total += part["bytes"]
    if total != archive["bytes"]:
        raise ValueError("Download parts do not add up to the approved ZIP")
    seen = set()
    rows = value.get("archives", [])
    if not isinstance(rows, list) or not 1 <= len(rows) <= core.MAX_ARCHIVES:
        raise ValueError("Invalid archive inventory")
    for row in rows:
        core._name(row.get("name"))
        if row["name"].casefold() in seen:
            raise ValueError("Duplicate archive inventory")
        seen.add(row["name"].casefold())
        if not isinstance(row.get("bytes"), int) or not 16 <= row["bytes"] <= core.MAX_ARCHIVE_BYTES:
            raise ValueError("Invalid member size")
        if not re.fullmatch(r"[a-f0-9]{64}", str(row.get("sha256", ""))):
            raise ValueError("Invalid member checksum")
    if pinned and (len(rows) != 310 or value.get("texture_count") != 23277):
        raise ValueError("Wrong release inventory")
    return value


def load_manifest(path, expected_sha256=None):
    path = core._plain(path)
    if path.stat().st_size > core.MAX_MANIFEST_BYTES:
        raise ValueError("Release manifest is too large")
    raw = path.read_bytes()
    if expected_sha256 and hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError("Bundled release manifest changed")
    return validate_manifest(core._json(raw))


def verify_zip(path, manifest, progress=None):
    """Pin whole bytes and the internal manifest before preparing any game files."""
    notify = progress or (lambda _: None)
    path = core._plain(path)
    expected = manifest["archive"]
    if path.stat().st_size != expected["bytes"]:
        raise ValueError("Choose the complete ZIP listed for this release")
    notify("Verifying the complete ZIP checksum…")
    with path.open("rb") as stream:
        if hash_stream(stream) != expected["sha256"]:
            raise ValueError("ZIP checksum differs from the approved TextureQuest release")
    core._zip_bounds(path)
    with zipfile.ZipFile(path) as archive:
        prefix = b"EQ2-Texture-Pack-v2\0"
        if not archive.comment.startswith(prefix):
            raise ValueError("Missing TextureQuest pack manifest")
        value = core._json(core._inflate(archive.comment[len(prefix):], core.MAX_MANIFEST_BYTES))
        if value.get("schema") != "eq2-texture-pack-v2" or value.get("pack_id") != core._digest({k: v for k, v in value.items() if k != "pack_id"}):
            raise ValueError("Invalid internal pack manifest")
        public = {r["name"]: (r["bytes"], r["sha256"]) for r in manifest["archives"]}
        internal_rows = value.get("archives", [])
        internal = {r["name"]: (r["bytes"], r["sha256"]) for r in internal_rows}
        if len(internal) != len(internal_rows) or internal != public:
            raise ValueError("Internal pack inventory differs from the release")
        infos = archive.infolist()
        if len(infos) != len(public) or {i.filename: i.file_size for i in infos} != {k: v[0] for k, v in public.items()}:
            raise ValueError("ZIP members differ from the release")
        for row in internal_rows:
            if row.get("member") != row["name"]:
                raise ValueError("Expected flat replacement archive names")
    return value


def verify_prepared(prepared, manifest):
    expected = {r["name"]: (r["bytes"], r["sha256"]) for r in manifest["archives"]}
    rows = prepared.get("archives", [])
    if len(rows) != len(expected) or {r["name"]: (r["bytes"], r["sha256"]) for r in rows} != expected:
        raise ValueError("Prepared bytes differ from the public release manifest")
    return prepared


def prepare_release(swapper, path, manifest, progress=None):
    notify = progress or (lambda _: None)
    verify_zip(path, manifest, notify)
    pointer = core._plain(swapper.state_dir / "texturequest-prepared.json", swapper.state_dir)
    if pointer.exists():
        try:
            prepared = verify_prepared(swapper._prepared(core._load(pointer)), manifest)
            for row in prepared["archives"]:
                notify("Verifying cached " + row["name"])
                target = core._plain(row["path"], swapper.state_dir / "prepared")
                if target.stat().st_size != row["bytes"] or core._hash(target) != row["sha256"]:
                    raise ValueError("Cached archive changed")
            return prepared
        except (ValueError, OSError, KeyError):
            # Do not erase unknown or damaged data automatically.
            raise ValueError("Prepared cache changed. Use Clear download/cache, then select the ZIP again.")
    required = sum(row["bytes"] for row in manifest["archives"]) + 64 * CHUNK
    if shutil.disk_usage(swapper.state_dir).free < required:
        raise OSError("Not enough space to prepare the complete pack. Free about 28 GB in the app's drive, or move this app to a larger drive.")
    prepared = verify_prepared(swapper.prepare([path], progress=notify), manifest)
    # Prepared objects already have a durable checksum/timestamp. Re-saving via
    # _save would mutate it and disagree with the core's original manifest.
    core._atomic(pointer, prepared)
    return prepared


def download(manifest, folder, progress=None, pause=None, opener=urlopen):
    """Resume one append-only ZIP; never retain an additional set of part files."""
    notify = progress or (lambda _text, _done, _total: None)
    folder = core._plain(folder)
    folder.mkdir(parents=True, exist_ok=True)
    name = manifest["archive"]["name"]
    if not re.fullmatch(r"(?:Texture-Pack|TextureQuest-Update-1\.[0-9]{1,4}\.[0-9]{1,4})\.zip", name):
        raise ValueError("Unexpected download output filename")
    final = core._plain(folder / name, folder)
    partial = core._plain(folder / (name + ".download"), folder)
    size = manifest["archive"]["bytes"]
    with core._lock(folder / "download.lock"):
        if final.exists():
            notify("Verifying existing download…", 0, size)
            if final.stat().st_size == size:
                with final.open("rb") as stream:
                    if hash_stream(stream, pause=pause) == manifest["archive"]["sha256"]:
                        return final
            raise ValueError("Existing Texture-Pack.zip differs from the release. Preserve or remove it before downloading.")
        if partial.exists() and partial.stat().st_size > size:
            raise ValueError("Partial download is larger than this release; preserve it and choose another folder")
        existing = partial.stat().st_size if partial.exists() else 0
        if shutil.disk_usage(folder).free < size - existing + 64 * CHUNK:
            raise OSError("Not enough free space for the download")
        with partial.open("r+b" if partial.exists() else "x+b") as out:
            start = 0
            for number, part in enumerate(manifest["parts"], 1):
                check_pause(pause)
                count = part["bytes"]
                available = max(0, min(count, existing - start))
                if available == count:
                    notify(f"Verifying saved part {number}/{len(manifest['parts'])}…", start, size)
                    out.seek(start)
                    if hash_stream(out, count, pause) == part["sha256"]:
                        start += count
                        continue
                    out.seek(start)
                    out.truncate()
                    existing = start
                    available = 0
                headers = {"User-Agent": "TextureQuest/1.1.0", "Accept-Encoding": "identity"}
                if available:
                    headers["Range"] = f"bytes={available}-"
                request = Request(part["url"], headers=headers)
                with opener(request, timeout=45) as response:
                    if urlsplit(response.geturl()).scheme != "https":
                        raise ValueError("Refusing a non-HTTPS download redirect")
                    status = response.getcode()
                    if status == 206:
                        expected_range = f"bytes {available}-{count - 1}/{count}"
                        if response.headers.get("Content-Range") != expected_range:
                            raise ValueError("Server returned a mismatched download range")
                    elif status == 200:
                        available = 0
                        out.seek(start)
                        out.truncate()
                    else:
                        raise ValueError(f"Download server returned HTTP {status}")
                    length = response.headers.get("Content-Length")
                    if length is not None and int(length) != count - available:
                        raise ValueError("Server returned an unexpected part size")
                    out.seek(start + available)
                    written = available
                    while written < count:
                        check_pause(pause)
                        chunk = response.read(min(CHUNK, count - written))
                        if not chunk:
                            raise OSError("Download interrupted. Download / Resume will continue it.")
                        out.write(chunk)
                        written += len(chunk)
                        notify(f"Downloading part {number}/{len(manifest['parts'])}", start + written, size)
                    if response.read(1):
                        raise ValueError("Download exceeded its expected size")
                out.flush()
                os.fsync(out.fileno())
                out.seek(start)
                if hash_stream(out, count, pause) != part["sha256"]:
                    out.seek(start)
                    out.truncate()
                    raise ValueError(f"Part {number} checksum failed. Resume to download that part again.")
                start += count
                existing = start
            out.flush()
            os.fsync(out.fileno())
            notify("Verifying the complete download…", size, size)
            out.seek(0)
            if hash_stream(out, pause=pause) != manifest["archive"]["sha256"]:
                raise ValueError("Complete download checksum failed; partial retained for inspection")
        os.replace(partial, final)
        notify("Download verified. Ready to prepare for installation.", size, size)
        return final
