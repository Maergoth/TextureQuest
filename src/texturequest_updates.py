"""Versioned complete-archive updates; reads game files but never installs them."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import time
import zipfile

from texture_swapper import core
import texturequest_release as release

VERSION = "1.1.0"  # Immutable texture package used by the 1.1 installer series.
UPDATE_NAME = f"TextureQuest-Update-{VERSION}.zip"
UPDATE_URL = f"https://github.com/Maergoth/TextureQuest/releases/download/v{VERSION}/"


def inventory(rows):
    if not isinstance(rows, list) or not 1 <= len(rows) <= core.MAX_ARCHIVES:
        raise ValueError("Invalid update inventory")
    result = {}
    for row in rows:
        core._name(row.get("name"))
        if row["name"].casefold() in {name.casefold() for name in result}:
            raise ValueError("Duplicate update archive")
        if (type(row.get("bytes")) is not int or not 16 <= row["bytes"] <= core.MAX_ARCHIVE_BYTES
                or not re.fullmatch(r"[0-9a-f]{64}", str(row.get("sha256", "")))
                or ("entries" in row and (type(row["entries"]) is not int or row["entries"] < 1))
                or row.get("member", row["name"]) != row["name"]):
            raise ValueError("Invalid update archive metadata")
        result[row["name"]] = (row["bytes"], row["sha256"])
    if sum(row[0] for row in result.values()) > core.MAX_TOTAL_BYTES:
        raise ValueError("Update inventory exceeds supported size")
    return result


def validate_manifest(value, pinned=True):
    version = value.get("version", "")
    if (value.get("schema") != "texturequest-release-v2"
            or not isinstance(version, str) or not re.fullmatch(r"1\.(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})", version)
            or tuple(map(int, version.split("."))) <= (1, 0, 0)):
        raise ValueError("Unsupported TextureQuest update")
    if value.get("base", {}).get("schema") != "texturequest-release-v1":
        raise ValueError("Update requires the immutable 1.0 base manifest")
    base = release.validate_manifest(value.get("base", {}), pinned=pinned)
    update = value.get("update", {})
    archive = update.get("archive", {})
    package_version = update.get("version", "")
    if (not isinstance(package_version, str)
            or not re.fullmatch(r"1\.(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})", package_version)
            or not (1, 0, 0) < tuple(map(int, package_version.split("."))) <= tuple(map(int, version.split(".")))):
        raise ValueError("Invalid texture package version")
    update_name = f"TextureQuest-Update-{package_version}.zip"
    update_url = f"https://github.com/Maergoth/TextureQuest/releases/download/v{package_version}/"
    if (archive.get("name") != update_name
            or type(archive.get("bytes")) is not int or archive["bytes"] <= 0
            or not re.fullmatch(r"[0-9a-f]{64}", str(archive.get("sha256", "")))):
        raise ValueError("Invalid update package")
    parts = update.get("parts", [])
    if not isinstance(parts, list) or not 1 <= len(parts) <= 100:
        raise ValueError("Invalid update download parts")
    for i, part in enumerate(parts, 1):
        name = update_name + f".{i:03d}"
        if (part.get("name") != name or part.get("url") != update_url + name
                or type(part.get("bytes")) is not int or not 0 < part["bytes"] < 2**31
                or not re.fullmatch(r"[0-9a-f]{64}", str(part.get("sha256", "")))):
            raise ValueError("Invalid update part or release URL")
    if sum(part["bytes"] for part in parts) != archive["bytes"]:
        raise ValueError("Update parts do not add up to the ZIP")
    original, changes, effective = map(inventory, (base["archives"], update.get("archives"), value.get("archives")))
    if not changes.keys() <= original.keys() or effective != {**original, **changes}:
        raise ValueError("Effective inventory must retain every unchanged base archive")
    if type(value.get("texture_count")) is not int or value["texture_count"] <= 0:
        raise ValueError("Invalid texture count")
    return value


def prepared_pointer(swapper, manifest):
    identity = core._digest(manifest)[:32]
    folder = core._plain(swapper.state_dir / "prepared" / identity, swapper.state_dir)
    return folder, folder / "manifest.json"


def matches(path, row, pause=None):
    path = core._plain(path)
    if not path.is_file() or path.stat().st_size != row["bytes"]:
        return False
    with path.open("rb") as stream:
        return release.hash_stream(stream, pause=pause) == row["sha256"]


def cached_sources(swapper, manifest, game=None, pause=None, progress=None):
    """Only matching generated release bytes can be reused; no stock backup."""
    notify = progress or (lambda _: None)
    expected = {row["name"].casefold(): row for row in manifest["archives"]}
    sources = {}
    prepared = core._plain(swapper.state_dir / "prepared", swapper.state_dir)
    if prepared.exists():
        for folder in sorted(prepared.iterdir()):
            release.check_pause(pause)
            if not re.fullmatch(r"[0-9a-f]{32}", folder.name):
                continue
            folder = core._plain(folder, prepared)
            receipt = core._plain(folder / "manifest.json", prepared)
            if not receipt.is_file():
                continue
            value = swapper._prepared(core._load(receipt))
            for row in value["archives"]:
                key = row["name"].casefold()
                wanted = expected.get(key)
                if key in sources or not wanted or (row["bytes"], row["sha256"]) != (wanted["bytes"], wanted["sha256"]):
                    continue
                notify("Verifying cached " + wanted["name"])
                path = core._plain(row["path"], folder)
                if not matches(path, wanted, pause):
                    raise ValueError("Prepared cache changed; preserve or clear it before continuing")
                sources[key] = (path, "cache")
    if game:
        paks = core.validate_game(game)
        if swapper.state_dir == paks.parent or swapper.state_dir.is_relative_to(paks.parent):
            raise ValueError("Installer data must stay outside the game")
        names = {}
        for path in paks.iterdir():
            if path.suffix.lower() in (".vpk", ".pak"):
                key = path.name.casefold()
                if key in names:
                    raise ValueError("Ambiguous game archive names")
                names[key] = path
        for key, row in expected.items():
            if key not in sources and key in names:
                notify("Checking installed " + row["name"])
                path = core._plain(names[key], paks)
                if matches(path, row, pause):
                    sources[key] = (path, "game")
    return sources


def _copy_checked(reader, target, row, pause):
    temporary = core._plain(target.with_name(target.name + ".pending"), target.parent)
    journal = core._plain(target.with_name(target.name + ".pending.json"), target.parent)
    binding = {key: row[key] for key in ("name", "bytes", "sha256")}
    if temporary.exists():
        if not journal.exists() or core._json(journal.read_bytes()) != binding:
            raise ValueError("Unknown interrupted archive; preserve it before retrying")
    elif journal.exists():
        if core._json(journal.read_bytes()) != binding:
            raise ValueError("Interrupted archive binding changed")
    else:
        core._atomic(journal, binding)
    digest, size = hashlib.sha256(), 0
    if temporary.exists():
        if temporary.stat().st_size > row["bytes"]:
            raise ValueError("Interrupted archive exceeds advertised size")
        with temporary.open("rb") as prefix:
            while chunk := prefix.read(core.CHUNK):
                release.check_pause(pause)
                if reader.read(len(chunk)) != chunk:
                    raise ValueError("Interrupted archive prefix changed; preserved for inspection")
                digest.update(chunk)
                size += len(chunk)
    with temporary.open("ab" if temporary.exists() else "xb") as out:
        while True:
            release.check_pause(pause)
            chunk = reader.read(core.CHUNK)
            if not chunk:
                break
            size += len(chunk)
            if size > row["bytes"]:
                raise ValueError("Archive exceeds advertised size")
            digest.update(chunk)
            out.write(chunk)
        out.flush()
        os.fsync(out.fileno())
    if size != row["bytes"] or digest.hexdigest() != row["sha256"]:
        raise ValueError("Archive checksum changed during preparation")
    os.replace(temporary, target)
    journal.unlink()


def prepare_update(swapper, manifest, game=None, base_zip=None, progress=None, pause=None,
                   opener=release.urlopen):
    """Prepare the full latest inventory; all game writes stay in explicit install."""
    validate_manifest(manifest, pinned=False)  # Whole manifest pin is checked on app load.
    notify = progress or (lambda _message, _done, _total: None)
    text = lambda message: notify(message, 0, 1)
    folder, receipt = prepared_pointer(swapper, manifest)
    with core._lock(swapper.state_dir / "operation.lock"):
        if receipt.exists():
            saved = release.verify_prepared(swapper._prepared(core._load(receipt)), manifest)
            for row in saved["archives"]:
                text("Verifying prepared " + row["name"])
                if not matches(Path(row["path"]), row, pause):
                    raise ValueError("Prepared update changed")
            return saved
        # An installer-only release can describe exactly the same pack. Reuse
        # the verified prepared receipt itself instead of duplicating 28 GB.
        prior_root = core._plain(swapper.state_dir / "prepared", swapper.state_dir)
        if prior_root.exists():
            for prior in sorted(prior_root.iterdir()):
                release.check_pause(pause)
                if not re.fullmatch(r"[0-9a-f]{32}", prior.name):
                    continue
                prior = core._plain(prior, prior_root)
                prior_receipt = core._plain(prior / "manifest.json", prior_root)
                if not prior_receipt.is_file():
                    continue
                saved = swapper._prepared(core._load(prior_receipt))
                try:
                    release.verify_prepared(saved, manifest)
                except ValueError:
                    continue
                for row in saved["archives"]:
                    text("Verifying saved " + row["name"])
                    if not matches(Path(row["path"]), row, pause):
                        raise ValueError("Prepared cache changed; preserve or clear it before continuing")
                core._atomic(swapper.state_dir / "texturequest-prepared.json", saved)
                notify("Saved complete pack verified. Click Install to apply it.", 1, 1)
                return saved
        sources = cached_sources(swapper, manifest, game, pause, text)
        folder.mkdir(parents=True, exist_ok=True)
        missing = []
        for row in manifest["archives"]:
            target = core._plain(folder / row["name"], folder)
            if target.exists():
                if not matches(target, row, pause):
                    raise ValueError("Incomplete preparation contains changed archive bytes")
                sources[row["name"].casefold()] = (target, "ready")
            elif row["name"].casefold() not in sources:
                missing.append(row)
        changed = {row["name"].casefold() for row in manifest["update"]["archives"]}
        required_packages = []
        if any(row["name"].casefold() not in changed for row in missing):
            required_packages.append(("base", manifest["base"]))
        if any(row["name"].casefold() in changed for row in missing):
            required_packages.append(("update", manifest["update"]))
        archives, opened, source_paths = {}, [], []
        try:
            for role, package in required_packages:
                release.check_pause(pause)
                if role == "base" and base_zip:
                    path = core._plain(base_zip)
                else:
                    downloads = core._plain(swapper.state_dir / "downloads" / package["version"], swapper.state_dir)
                    # Reuse the original installer's single saved base ZIP.
                    legacy = swapper.state_dir / "downloads/Texture-Pack.zip"
                    if role == "base" and legacy.is_file():
                        path = core._plain(legacy, swapper.state_dir)
                    else:
                        path = release.download(package, downloads, notify, pause, opener)
                release.verify_zip(path, package, text)
                source_paths.append(str(path))
                opened.append(zipfile.ZipFile(path))
                archives[role] = opened[-1]
            records = []
            for row in manifest["archives"]:
                release.check_pause(pause)
                target = core._plain(folder / row["name"], folder)
                key = row["name"].casefold()
                text("Preparing " + row["name"])
                if not target.exists():
                    source = sources.get(key)
                    if shutil.disk_usage(folder).free < row["bytes"] + 64 * core.CHUNK:
                        raise OSError("Not enough free space to prepare " + row["name"])
                    if source:
                        # Keep the core's no-hardlinks rule. Installed bytes may be
                        # changed by the patcher; prior caches remain recoverable.
                        with source[0].open("rb") as stream:
                            _copy_checked(stream, target, row, pause)
                    else:
                        role = "update" if key in changed else "base"
                        with archives[role].open(row["name"]) as stream:
                            _copy_checked(stream, target, row, pause)
                if not matches(target, row, pause):
                    raise ValueError("Prepared archive changed")
                entries = core.validate_archive(target)
                if "entries" in row and len(entries) != row["entries"]:
                    raise ValueError("Prepared archive entry count differs")
                records.append(dict(name=row["name"], path=str(target), bytes=row["bytes"],
                                    sha256=row["sha256"], entries=len(entries)))
            value = dict(schema=core.SCHEMA + "-prepared", id=folder.name, created_at=core._now(),
                         archives=records, total_bytes=sum(row["bytes"] for row in records),
                         source_paths=source_paths, texturequest_version=manifest["version"],
                         release_sha256=core._digest(manifest))
            core._save(receipt, value)
            release.verify_prepared(swapper._prepared(value), manifest)
            core._atomic(swapper.state_dir / "texturequest-prepared.json", value)
            notify("Complete pack verified. Click Install to apply it.", 1, 1)
            return value
        finally:
            for archive in opened:
                archive.close()
