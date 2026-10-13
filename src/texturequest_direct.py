"""One verified VPK cache for direct release downloads and explicit installs.

Legacy prepared files move only within this app's data directory. Unknown
downloads, journals and interrupted bytes are preserved for recovery.
"""
from __future__ import annotations

from contextlib import ExitStack
import copy
import hashlib
import os
from pathlib import Path
import re
import shutil
import uuid
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from texture_swapper import core
import texturequest_release as release

SCHEMA = "texturequest-direct-prepared-v1"
REPOSITORY = "https://github.com/Maergoth/TextureQuest"
CHECKPOINT = 8 * 1024**2
# Published immutable containers. 1.1.1 reused the 1.1.0 texture package.
# Their original release manifests and delivery proofs remain preserved.
LEGACY_PACKAGES = (
    {"version": "1.0.0", "name": "Texture-Pack.zip", "bytes": 27763745032,
     "sha256": "9501071759d57c26175ee9245b3f5dfee4d4cee7f19a57c039289d09d2041caf"},
    {"version": "1.1.0", "name": "TextureQuest-Update-1.1.0.zip", "bytes": 1041986839,
     "sha256": "7c80130a1cdb33b7fd44ac9d770249b4fe4781ef81e4237b53cffde023679f02"},
    {"version": "1.2.0", "name": "TextureQuest-Update-1.2.0.zip", "bytes": 1624207428,
     "sha256": "1a5fca080a48949d98a73833c3b4dba849e99e45a9e5069377890cf2aa6bd375"},
)


def _version(value):
    if not isinstance(value, str) or not re.fullmatch(r"1\.(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})", value):
        raise ValueError("Unsupported direct archive version")
    return tuple(map(int, value.split(".")))


def validate_manifest(value, pinned=True):
    if not isinstance(value, dict) or set(value) != {"schema", "version", "texture_version", "texture_count", "archives"}:
        raise ValueError("Unsupported direct archive manifest")
    if value["schema"] != "texturequest-release-v3" or _version(value["version"]) < (1, 3, 0):
        raise ValueError("Unsupported direct archive release")
    if not (1, 0, 0) <= _version(value["texture_version"]) <= _version(value["version"]):
        raise ValueError("Invalid direct texture version")
    rows = value["archives"]
    if not isinstance(rows, list) or not 1 <= len(rows) <= core.MAX_ARCHIVES or (pinned and len(rows) != 310):
        raise ValueError("Invalid complete direct archive inventory")
    if type(value["texture_count"]) is not int or value["texture_count"] <= 0:
        raise ValueError("Invalid direct texture count")
    names = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"name", "bytes", "sha256", "entries", "url"}:
            raise ValueError("Invalid direct archive identity")
        name = core._name(row["name"])
        if (name.casefold() in names or type(row["bytes"]) is not int or not 16 <= row["bytes"] < 2**31
                or row["bytes"] > core.MAX_ARCHIVE_BYTES or type(row["entries"]) is not int or row["entries"] < 1
                or not isinstance(row["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", row["sha256"])):
            raise ValueError("Invalid direct archive metadata")
        names.add(name.casefold())
        prefix = REPOSITORY + "/releases/download/v"
        url = row["url"]
        if not isinstance(url, str) or not url.startswith(prefix) or not url.endswith("/" + name):
            raise ValueError("Unexpected direct archive URL")
        tag = url[len(prefix):-(len(name) + 1)]
        if not (1, 3, 0) <= _version(tag) <= _version(value["version"]) or url != prefix + tag + "/" + name:
            raise ValueError("Invalid immutable direct archive URL")
    if sum(row["bytes"] for row in rows) > core.MAX_TOTAL_BYTES:
        raise ValueError("Direct pack exceeds the supported size")
    return value


def _identity(row):
    return {key: row[key] for key in ("name", "bytes", "sha256", "entries")}


def _matches(path, row, pause=None):
    path = core._plain(path)
    if not path.is_file() or path.stat().st_size != row["bytes"]:
        return False
    with path.open("rb") as stream:
        return release.hash_stream(stream, pause=pause) == row["sha256"]


def _checked_archive(path, row, pause=None):
    if not _matches(path, row, pause):
        raise ValueError("Archive checksum differs: " + row["name"])
    release.check_pause(pause)
    if len(core.validate_archive(path)) != row["entries"]:
        raise ValueError("Archive index differs: " + row["name"])


class DirectSwapper(core.Swapper):
    """Inherit game locks/staging/install guards; specialize only cache custody."""
    def _prepared(self, value):
        if not isinstance(value, dict) or value.get("schema") != SCHEMA:
            return super()._prepared(value)
        saved = core._load(core._plain(self.state_dir / "direct-prepared.json", self.state_dir))
        if saved != value:
            raise ValueError("Direct prepared receipt changed")
        manifest = validate_manifest(saved.get("release", {}))
        rows = saved.get("archives")
        expected = [{**_identity(row), "path": str(self.state_dir / "downloads" / row["name"])} for row in manifest["archives"]]
        if (rows != expected or saved.get("total_bytes") != sum(row["bytes"] for row in expected)
                or saved.get("release_sha256") != core._digest(manifest)
                or saved.get("id") != core._digest([_identity(row) for row in manifest["archives"]])[:32]):
            raise ValueError("Direct prepared inventory changed")
        for row in rows:
            if core._plain(row["path"], self.state_dir / "downloads") != self.state_dir / "downloads" / row["name"]:
                raise ValueError("Direct prepared path changed")
        return saved

    def install(self, prepared, game_dir, progress=None):
        with core._lock(self.state_dir / "sideload/helper-owner.lock"):
            return super().install(prepared, game_dir, progress)


def _resume_journal(path, row, partial):
    if not path.exists():
        if partial.exists():
            raise ValueError("Unknown interrupted download is preserved: " + partial.name)
        value = {"schema": "texturequest-direct-download-v1", "archive": dict(row),
                 "bytes_written": 0, "prefix_sha256": hashlib.sha256(b"").hexdigest()}
        core._atomic(path, value)
        return value
    if path.stat().st_size > core.MAX_MANIFEST_BYTES:
        raise ValueError("Download journal exceeds supported bounds")
    value = core._json(path.read_bytes())
    if (not isinstance(value, dict) or set(value) != {"schema", "archive", "bytes_written", "prefix_sha256"}
            or value["schema"] != "texturequest-direct-download-v1" or value["archive"] != row
            or type(value["bytes_written"]) is not int or not 0 <= value["bytes_written"] <= row["bytes"]
            or not isinstance(value["prefix_sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", value["prefix_sha256"])):
        raise ValueError("Unknown or changed download journal is preserved")
    if not partial.exists() and value["bytes_written"]:
        raise ValueError("Journaled download bytes are missing")
    if partial.exists() and (not partial.is_file() or partial.stat().st_size != value["bytes_written"]):
        raise ValueError("Interrupted download length changed; preserved for recovery")
    return value


def _download(row, folder, old=None, notify=None, pause=None, opener=urlopen):
    notify = notify or (lambda *_: None)
    folder = core._plain(folder)
    target = core._plain(folder / row["name"], folder)
    partial = core._plain(folder / (row["name"] + ".pending"), folder)
    journal = core._plain(folder / (row["name"] + ".pending.json"), folder)
    if target.exists():
        if _matches(target, row, pause):
            if len(core.validate_archive(target)) != row["entries"]:
                raise ValueError("Archive index differs: " + row["name"])
            return target
        if old is None or not _matches(target, old, pause):
            raise ValueError("Unrecognized cached archive is preserved: " + target.name)
    saved = _resume_journal(journal, row, partial)
    digest = hashlib.sha256()
    if partial.exists():
        with partial.open("rb") as stream:
            while chunk := stream.read(core.CHUNK):
                release.check_pause(pause)
                digest.update(chunk)
    if digest.hexdigest() != saved["prefix_sha256"]:
        raise ValueError("Interrupted download prefix changed; preserved for recovery")
    count = saved["bytes_written"]
    if shutil.disk_usage(folder).free < row["bytes"] - count + 64 * core.CHUNK:
        raise OSError("Not enough free space for " + row["name"])
    if count < row["bytes"]:
        headers = {"User-Agent": "TextureQuest/1.3.0", "Accept-Encoding": "identity"}
        if count:
            headers["Range"] = f"bytes={count}-"
        with opener(Request(row["url"], headers=headers), timeout=45) as response:
            final = urlsplit(response.geturl())
            if (final.scheme != "https" or final.username or final.password or final.port not in (None, 443)
                    or final.hostname not in {"github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}):
                raise ValueError("Unexpected direct archive response")
            status = response.getcode()
            if status == 206:
                if response.headers.get("Content-Range") != f"bytes {count}-{row['bytes'] - 1}/{row['bytes']}":
                    raise ValueError("Direct archive range differs")
                received = count
            elif status == 200:
                received = 0
            else:
                raise OSError("Archive server returned HTTP " + str(status))
            length = response.headers.get("Content-Length")
            if length is not None and int(length) != row["bytes"] - received:
                raise ValueError("Direct archive response length differs")
            if not received and count:
                with partial.open("rb") as prefix:
                    left = count
                    while left:
                        release.check_pause(pause)
                        expected = prefix.read(min(core.CHUNK, left))
                        if response.read(len(expected)) != expected:
                            raise ValueError("Server resume prefix differs; saved bytes preserved")
                        left -= len(expected)
            with partial.open("ab" if partial.exists() else "xb") as output:
                while count < row["bytes"]:
                    release.check_pause(pause)
                    maximum = min(CHECKPOINT, row["bytes"] - count)
                    chunk = response.read(maximum)
                    if not chunk:
                        raise OSError("Download interrupted; resume will use the verified prefix")
                    if len(chunk) > maximum:
                        raise ValueError("Archive response exceeded the requested bound")
                    output.write(chunk)
                    output.flush()
                    os.fsync(output.fileno())
                    digest.update(chunk)
                    count += len(chunk)
                    saved.update(bytes_written=count, prefix_sha256=digest.hexdigest())
                    core._atomic(journal, saved)
                    notify("Downloading " + row["name"], count, row["bytes"])
                if response.read(1):
                    raise ValueError("Archive response exceeds advertised length")
    _checked_archive(partial, row, pause)
    if target.exists() and (old is None or not _matches(target, old, pause)):
        raise ValueError("Cached archive changed before replacement; preserved")
    release.check_pause(pause)
    os.replace(core._plain(partial, folder), core._plain(target, folder))
    journal.unlink()
    return target


def _migration(swapper, source, row, notify, pause):
    """A journaled same-drive move, including exact duplicate coalescing."""
    folder = core._plain(swapper.state_dir / "downloads", swapper.state_dir)
    target = core._plain(folder / row["name"], folder)
    source = core._plain(source, swapper.state_dir / "prepared")
    journal = core._plain(folder / (row["name"] + ".migration.json"), folder)
    binding = {"schema": "texturequest-direct-migration-v1", "source": str(source),
               "target": str(target), "archive": _identity(row)}
    if journal.exists():
        if journal.stat().st_size > core.MAX_MANIFEST_BYTES or core._json(journal.read_bytes()) != binding:
            raise ValueError("Unknown cache migration is preserved")
    else:
        core._atomic(journal, binding)
    _checked_archive(source, row, pause)
    if target.exists():
        _checked_archive(target, row, pause)
    release.check_pause(pause)
    notify("Moving saved " + row["name"], 0, 1)
    os.replace(core._plain(source, swapper.state_dir / "prepared"), core._plain(target, folder))
    receipt = core._plain(swapper.state_dir / "cache-migrations" / (uuid.uuid4().hex + ".json"), swapper.state_dir)
    core._atomic(receipt, dict(binding, completed_at=core._now()))
    journal.unlink()


def _recover_migrations(swapper, rows, pause):
    folder = core._plain(swapper.state_dir / "downloads", swapper.state_dir)
    for row in rows:
        journal = core._plain(folder / (row["name"] + ".migration.json"), folder)
        if not journal.exists():
            continue
        if journal.stat().st_size > core.MAX_MANIFEST_BYTES:
            raise ValueError("Unknown migration journal size")
        saved = core._json(journal.read_bytes())
        target = core._plain(folder / row["name"], folder)
        if (not isinstance(saved, dict) or set(saved) != {"schema", "source", "target", "archive"}
                or saved["schema"] != "texturequest-direct-migration-v1" or saved["archive"] != _identity(row)
                or saved["target"] != str(target)):
            raise ValueError("Unknown migration journal is preserved")
        source = core._plain(saved["source"], swapper.state_dir / "prepared")
        if (source.name != row["name"] or source.parent.parent != swapper.state_dir / "prepared"
                or not re.fullmatch(r"[0-9a-f]{32}", source.parent.name)):
            raise ValueError("Migration source path differs")
        owner = core.Swapper._prepared(swapper, core._load(source.parent / "manifest.json"))
        if not any(_identity(item) == _identity(row) and item["path"] == str(source) for item in owner["archives"]):
            raise ValueError("Migration is not bound to a recognized prepared archive")
        if source.exists():
            _migration(swapper, source, row, lambda *_: None, pause)
        else:
            _checked_archive(target, row, pause)
            receipt = core._plain(swapper.state_dir / "cache-migrations" / (uuid.uuid4().hex + ".json"), swapper.state_dir)
            core._atomic(receipt, dict(saved, completed_at=core._now()))
            journal.unlink()


def _remove_exact_download(path, package, pause):
    """On Windows, delete the exact fully hashed file through its retained handle.

    Denying write/delete sharing prevents a pathname swap or mutation between
    hash verification and deletion. Failure leaves the download intact.
    """
    path = core._plain(path)
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        import msvcrt
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                     wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.SetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        kernel.SetFileInformationByHandle.restype = wintypes.BOOL
        handle = kernel.CreateFileW(str(path), 0x80000000 | 0x10000, 1, None, 3, 0x80, None)
        if not handle or handle == wintypes.HANDLE(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            fd = msvcrt.open_osfhandle(int(handle), os.O_RDONLY | os.O_BINARY)
        except BaseException:
            kernel.CloseHandle(handle)
            raise
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if info.st_nlink != 1 or info.st_size != package["bytes"]:
                return False
            if release.hash_stream(stream, pause=pause) != package["sha256"]:
                return False
            release.check_pause(pause)
            core._plain(path)
            class Disposition(ctypes.Structure):
                _fields_ = [("DeleteFile", ctypes.c_ubyte)]
            disposition = Disposition(1)
            if not kernel.SetFileInformationByHandle(handle, 4, ctypes.byref(disposition), ctypes.sizeof(disposition)):
                raise ctypes.WinError(ctypes.get_last_error())
        return True
    # Portable author fixtures; the consumer build is Windows-only.
    with path.open("rb") as stream:
        info = os.fstat(stream.fileno())
        if info.st_nlink != 1 or info.st_size != package["bytes"] or release.hash_stream(stream, pause=pause) != package["sha256"]:
            return False
        current = core._plain(path).stat()
        if (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns) != (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns):
            raise ValueError("Legacy download changed before deletion")
        release.check_pause(pause)
        path.unlink()
    return True


def _retire_legacy_downloads(swapper, prepared, notify, pause, idle):
    """Called only while operation, helper and download locks remain held."""
    downloads = core._plain(swapper.state_dir / "downloads", swapper.state_dir)
    candidates = [(downloads / item["version"] / item["name"], item) for item in LEGACY_PACKAGES]
    candidates.insert(0, (downloads / "Texture-Pack.zip", LEGACY_PACKAGES[0]))
    candidates = [(path, item) for path, item in candidates if path.exists() or path.is_symlink()]
    if not candidates:
        return
    # Complete new custody must exist and be verified before reclaiming bytes.
    swapper._prepared(prepared)
    if core._load(swapper.state_dir / "texturequest-prepared.json") != prepared:
        raise ValueError("Current direct cache pointer changed")
    for row in prepared["archives"]:
        idle()
        notify("Verifying ready cache before reclaiming downloads: " + row["name"], 0, 1)
        _checked_archive(row["path"], row, pause)
    result = {"schema": "texturequest-legacy-download-retirement-v1", "prepared_id": prepared["id"],
              "release_sha256": prepared["release_sha256"], "created_at": core._now(), "files": []}
    receipt = core._plain(swapper.state_dir / "cache-migrations" / (uuid.uuid4().hex + "-legacy-downloads.json"), swapper.state_dir)
    core._save(receipt, result)
    with ExitStack() as locks:
        # Old installers lock their version directories; take the same locks.
        for folder in sorted({path.parent for path, _ in candidates} - {downloads}):
            locks.enter_context(core._lock(core._plain(folder / "download.lock", downloads)))
        for path, item in candidates:
            idle()
            release.check_pause(pause)
            record = {**item, "path": str(path), "status": "planned"}
            result["files"].append(record)
            core._save(receipt, result)
            notify("Reclaiming verified legacy download: " + item["name"], 0, 1)
            try:
                path = core._plain(path, downloads)
                if not path.is_file():
                    record["status"] = "preserved-unknown"
                else:
                    record["status"] = "removed" if _remove_exact_download(path, item, pause) else "preserved-mismatch"
            except (OSError, ValueError) as error:
                # Busy, linked, changed or foreign bytes never justify removal.
                record.update(status="preserved-unknown", reason=type(error).__name__)
            core._save(receipt, result)
    result.update(completed_at=core._now(), removed_bytes=sum(row["bytes"] for row in result["files"] if row["status"] == "removed"))
    core._save(receipt, result)


def prepare_direct(swapper, manifest, progress=None, pause=None, opener=urlopen, cache_in_use=None):
    """Verify/download ready VPKs. Game files change only through explicit install."""
    manifest = copy.deepcopy(manifest)
    validate_manifest(manifest)
    notify = progress or (lambda *_: None)
    if cache_in_use is not None and not callable(cache_in_use):
        raise ValueError("Invalid cache ownership guard")
    def idle():
        if cache_in_use is not None and cache_in_use():
            raise RuntimeError("Stop sideload before changing the saved texture cache")
    idle()
    folder = core._plain(swapper.state_dir / "downloads", swapper.state_dir)
    with ExitStack() as locks:
        locks.enter_context(core._lock(swapper.state_dir / "operation.lock"))
        locks.enter_context(core._lock(swapper.state_dir / "sideload/helper-owner.lock"))
        idle()
        folder.mkdir(parents=True, exist_ok=True)
        locks.enter_context(core._lock(folder / "download.lock"))
        _recover_migrations(swapper, manifest["archives"], pause)
        previous = {}
        receipt = core._plain(swapper.state_dir / "direct-prepared.json", swapper.state_dir)
        if receipt.exists():
            old = swapper._prepared(core._load(receipt))
            previous = {row["name"].casefold(): row for row in old["archives"]}
        legacy = core._plain(swapper.state_dir / "prepared", swapper.state_dir)
        if legacy.exists():
            children = list(legacy.iterdir())
            if len(children) > core.MAX_ARCHIVES:
                raise ValueError("Too many saved cache directories; preserve them for review")
            for child in sorted(children):
                release.check_pause(pause)
                if not re.fullmatch(r"[0-9a-f]{32}", child.name):
                    continue
                child = core._plain(child, legacy)
                old_receipt = core._plain(child / "manifest.json", legacy)
                if not old_receipt.is_file():
                    continue
                try:
                    old = core.Swapper._prepared(swapper, core._load(old_receipt))
                except (ValueError, OSError, KeyError, TypeError):
                    continue  # Unknown receipts are not custody evidence.
                wanted = {row["name"].casefold(): row for row in manifest["archives"]}
                matches = [row for row in old["archives"] if row["name"].casefold() in wanted and
                           _identity(row) == _identity(wanted[row["name"].casefold()]) and Path(row["path"]).exists()]
                if not matches:
                    continue
                # Verify the complete recognized legacy inventory before any move.
                for row in old["archives"]:
                    if Path(row["path"]).exists():
                        notify("Verifying saved " + row["name"], 0, 1)
                        _checked_archive(row["path"], row, pause)
                    elif not _matches(folder / row["name"], row, pause):
                        raise ValueError("Saved legacy inventory is incomplete; preserve for recovery")
                for row in matches:
                    idle()
                    _migration(swapper, Path(row["path"]), wanted[row["name"].casefold()], notify, pause)
        records = []
        for number, row in enumerate(manifest["archives"], 1):
            release.check_pause(pause)
            idle()
            notify(f"Verifying archive {number}/{len(manifest['archives'])}: " + row["name"], 0, 1)
            path = _download(row, folder, previous.get(row["name"].casefold()), notify, pause, opener)
            records.append(dict(_identity(row), path=str(path)))
        value = {"schema": SCHEMA, "id": core._digest([_identity(row) for row in manifest["archives"]])[:32],
                 "created_at": core._now(), "archives": records, "total_bytes": sum(row["bytes"] for row in records),
                 "source_paths": [], "release": manifest, "release_sha256": core._digest(manifest)}
        core._save(receipt, value)
        swapper._prepared(value)
        core._atomic(core._plain(swapper.state_dir / "texturequest-prepared.json", swapper.state_dir), value)
        _retire_legacy_downloads(swapper, value, notify, pause, idle)
        notify("Ready VPKs verified. Click Install to apply them.", 1, 1)
        return value
