"""Portable, bounded EQ2 pack installation. No AI imports, backups or sideloads.

Install is an explicit API action. Receipts are committed before any destination
mutation, so recovery also covers an interrupted install. Installed archives
stay writable; the official patcher restores their original contents.
"""
from __future__ import annotations

from contextlib import contextmanager
from collections import OrderedDict
from datetime import datetime, timezone
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import time
import threading
import uuid
import zipfile
import zlib

MAX_ARCHIVES = 2048
MAX_ARCHIVE_BYTES = 4 * 1024**3
MAX_TOTAL_BYTES = 128 * 1024**3
MAX_ENTRY_BYTES = 512 * 1024**2
MAX_INDEX_BYTES = 64 * 1024**2
MAX_MANIFEST_BYTES = 8 * 1024**2
MAX_ENTRIES = 500000
CHUNK = 1024**2
SCHEMA = "eq2-texture-swapper-v1"
RESERVED = {"CON", "PRN", "AUX", "NUL", *[f"{p}{n}" for p in ("COM", "LPT") for n in range(1, 10)]}
_INDEX_CACHE = OrderedDict()
_INDEX_LOCK = threading.Lock()


def _now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _digest(value):
    raw = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode()
    return hashlib.sha256(raw).hexdigest()


def _json(raw):
    def pairs(rows):
        value = {}
        for key, item in rows:
            if key in value:
                raise ValueError("Duplicate JSON key")
            value[key] = item
        return value
    return json.loads(raw, object_pairs_hook=pairs,
                      parse_constant=lambda v: (_ for _ in ()).throw(ValueError("Invalid JSON number")))


def _plain(path, root=None):
    path = Path(os.path.abspath(path))
    if root is not None and not path.is_relative_to(Path(os.path.abspath(root))):
        raise ValueError("Path escaped its allowed directory")
    for part in (path, *path.parents):
        if part.exists() or part.is_symlink():
            info = part.lstat()
            if part.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError("Symbolic links and junctions are not supported")
            if part.is_file() and info.st_nlink > 1:
                raise ValueError("Hard-linked files are not supported")
    return path


def _member(name, allow_directory=False):
    if not isinstance(name, str) or not name or len(name) > 4096 or "\\" in name:
        raise ValueError("Unsafe archive member name")
    if allow_directory and name.endswith("/"):
        name = name[:-1]
    parts = name.split("/")
    if any(not p or p in (".", "..") or p[-1:] in (" ", ".") or
           any(ord(c) < 32 or c in '<>:"|?*' for c in p) or
           p.split(".")[0].upper() in RESERVED for p in parts):
        raise ValueError("Unsafe archive member path")
    return parts


def _name(name):
    if not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z0-9_. -]+\.(?:vpk|pak)", name, re.I):
        raise ValueError("Select simple .vpk or .pak filenames")
    if len(_member(name)) != 1:
        raise ValueError("Archive destination must be a basename")
    return name


def _hash(path):
    result = hashlib.sha256()
    with _plain(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(CHUNK), b""):
            result.update(chunk)
    return result.hexdigest()


def _inflate(raw, limit):
    try:
        decoder = zlib.decompressobj()
        value = decoder.decompress(raw, limit + 1)
    except zlib.error as error:
        raise ValueError("Malformed EQ2 compressed stream") from error
    if len(value) > limit or decoder.unconsumed_tail or not decoder.eof or decoder.unused_data:
        raise ValueError("EQ2 compressed stream exceeded its bounds or has trailing data")
    return value


def _index(stream, size):
    if not 16 <= size <= MAX_ARCHIVE_BYTES:
        raise ValueError("Unsupported EQ2 archive size")
    stream.seek(size - 12)
    magic, offset, version = struct.unpack("<III", stream.read(12))
    if magic != 0x33744 or version != 2 or offset + 4 > size - 12:
        raise ValueError("Not a supported EverQuest II VPK archive")
    length = size - offset - 16
    if not 1 <= length <= MAX_INDEX_BYTES:
        raise ValueError("EQ2 index size is out of bounds")
    stream.seek(offset)
    if struct.unpack("<I", stream.read(4))[0] != length + 12:
        raise ValueError("EQ2 index size field changed")
    raw = _inflate(stream.read(length), MAX_INDEX_BYTES)
    if len(raw) < 8:
        raise ValueError("Truncated EQ2 index")
    raw_size, count = struct.unpack_from("<II", raw)
    if raw_size != len(raw) or not 1 <= count <= MAX_ENTRIES:
        raise ValueError("Invalid EQ2 index inventory")
    rows, names, cursor, end = [], set(), 8, 0
    for _ in range(count):
        if cursor + 12 > len(raw):
            raise ValueError("Truncated EQ2 index")
        block, compressed, name_size = struct.unpack_from("<III", raw, cursor)
        cursor += 12
        if not 1 <= name_size <= 4096 or cursor + name_size > len(raw):
            raise ValueError("Invalid EQ2 resource name length")
        name = raw[cursor:cursor + name_size].decode("latin-1")
        cursor += name_size
        canonical = re.sub(r"/{2,}", "/", name)
        _member(canonical)
        if canonical != name and name.lower().endswith(".dds"):
            raise ValueError("DDS names must be exact")
        if canonical.casefold() in names or block != end or not 1 <= compressed <= MAX_ENTRY_BYTES:
            raise ValueError("Duplicate resource or invalid EQ2 block offsets")
        names.add(canonical.casefold())
        end = block + 4 + compressed
        if end > offset:
            raise ValueError("EQ2 block overlaps its index")
        stream.seek(block)
        if struct.unpack("<I", stream.read(4))[0] != compressed:
            raise ValueError("EQ2 block size differs from its index")
        rows.append(dict(name=name, offset=block, csize=compressed, bytes=None))
    if cursor != len(raw) or end != offset:
        raise ValueError("Unindexed or trailing EQ2 archive data")
    return rows


def _payload(stream, row, collect=False):
    """Validate a complete zlib stream with a bounded working buffer."""
    stream.seek(row["offset"] + 4)
    decoder = zlib.decompressobj()
    remaining, expanded, prefix, chunks = row["csize"], 0, bytearray(), []
    while remaining:
        raw = stream.read(min(CHUNK, remaining))
        if not raw:
            raise ValueError("Truncated EQ2 block")
        remaining -= len(raw)
        while raw:
            try:
                piece = decoder.decompress(raw, CHUNK)
            except zlib.error as error:
                raise ValueError("Malformed EQ2 block") from error
            expanded += len(piece)
            if expanded > MAX_ENTRY_BYTES + 4105:
                raise ValueError("Expanded EQ2 resource exceeds its limit")
            if len(prefix) < 4105:
                prefix.extend(piece[:4105 - len(prefix)])
            if collect:
                chunks.append(piece)
            if decoder.unused_data:
                raise ValueError("Trailing data in EQ2 block")
            raw = decoder.unconsumed_tail
    if not decoder.eof or len(prefix) < 8:
        raise ValueError("Unterminated or truncated EQ2 block")
    name_size, data_size = struct.unpack_from("<II", prefix)
    if not 1 <= name_size <= 4097 or data_size > MAX_ENTRY_BYTES or expanded != 8 + name_size + data_size:
        raise ValueError("EQ2 payload lengths differ from the block header")
    embedded = bytes(prefix[8:8 + name_size])
    exact = row["name"].encode("latin-1") + b"\0"
    alias = re.sub(r"/{2,}", "/", row["name"]).encode("latin-1") + b"\0"
    if embedded not in (exact, alias):
        raise ValueError("EQ2 resource name differs from its index")
    row["bytes"] = data_size
    return b"".join(chunks)[8 + name_size:] if collect else data_size


def archive_index(path):
    """Return bounded, structurally checked native EQ2 resource records."""
    path = _plain(path)
    with path.open("rb") as stream:
        return [dict(row) for row in _cached_index(path, stream)]


def _cached_index(path, stream):
    info = os.fstat(stream.fileno())
    key = (str(path), info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)
    with _INDEX_LOCK:
        found = _INDEX_CACHE.get(key)
        if found is not None:
            _INDEX_CACHE.move_to_end(key)
            return found
    rows = _index(stream, info.st_size)
    with _INDEX_LOCK:
        _INDEX_CACHE[key] = rows
        while len(_INDEX_CACHE) > 16:
            _INDEX_CACHE.popitem(last=False)
    return rows


def archive_read(path, entry_name):
    path = _plain(path)
    with path.open("rb") as stream:
        rows = _cached_index(path, stream)
        row = next((item for item in rows if item["name"] == entry_name), None)
        if row is None:
            raise KeyError(entry_name)
        return _payload(stream, dict(row), collect=True)


def validate_archive(path):
    path = _plain(path)
    with path.open("rb") as stream:
        rows = _index(stream, path.stat().st_size)
        total = 0
        for row in rows:
            total += _payload(stream, row)
            if total > MAX_TOTAL_BYTES:
                raise ValueError("EQ2 expanded archive exceeds its limit")
    return rows


def _readonly(path):
    return _readonly_info(_plain(path).stat())


def _readonly_info(info):
    if os.name == "nt":
        return bool(info.st_file_attributes & stat.FILE_ATTRIBUTE_READONLY)
    return not bool(info.st_mode & stat.S_IWUSR)


def _file_signature(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mode, info.st_mtime_ns,
            info.st_ctime_ns, getattr(info, "st_file_attributes", 0))


def _installed_integrity(target, row):
    """Read current bytes without following redirected paths or trusting a stale receipt."""
    result = dict(row, exists=None, readonly=None, actual_sha256=None,
                  actual_bytes=None, integrity="unverified", verification_error=None)
    try:
        target = _plain(target)
        try:
            before = target.stat()
        except FileNotFoundError:
            result.update(exists=False, readonly=False, integrity="missing")
            return result
        result["exists"] = True
        if not stat.S_ISREG(before.st_mode):
            raise ValueError("Managed archive is no longer a regular file")
        if not row.get("managed"):
            result.update(readonly=_readonly_info(before), actual_bytes=before.st_size,
                          integrity="inactive")
            return result
        digest = _hash(target)
        after = _plain(target).stat()
        if _file_signature(before) != _file_signature(after):
            raise OSError("Archive changed during verification; refresh after the patcher finishes")
        intact = digest == row["sha256"] and after.st_size == row["bytes"]
        result.update(readonly=_readonly_info(after), actual_sha256=digest,
                      actual_bytes=after.st_size, integrity="intact" if intact else "changed")
    except (OSError, ValueError) as error:
        result.update(integrity="unverified", readonly=None, actual_sha256=None,
                      actual_bytes=None, verification_error=str(error))
    return result


def _set_readonly(path, enabled):
    path = _plain(path)
    info = path.stat()
    # Windows chmod only changes FILE_ATTRIBUTE_READONLY, preserving other flags.
    os.chmod(path, (info.st_mode & ~0o222) if enabled else (info.st_mode | stat.S_IWUSR))
    if _readonly(path) != enabled:
        raise OSError("Unable to update the archive read-only flag")


def _atomic(path, value):
    path = _plain(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = _plain(path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp"))
    try:
        with temp.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        for attempt in range(15):
            try:
                os.replace(temp, path)
                break
            except PermissionError:
                if attempt == 14:
                    raise
                time.sleep(0.05)
    finally:
        if temp.exists():
            temp.unlink()


def _save(path, value):
    value["updated_at"] = _now()
    value["record_sha256"] = _digest({k: v for k, v in value.items() if k != "record_sha256"})
    _atomic(path, value)


def _load(path):
    if _plain(path).stat().st_size > MAX_MANIFEST_BYTES:
        raise ValueError("Swapper manifest is too large")
    value = _json(path.read_bytes())
    if value.get("record_sha256") != _digest({k: v for k, v in value.items() if k != "record_sha256"}):
        raise ValueError("Swapper manifest checksum changed; preserve this file for recovery")
    return value


@contextmanager
def _lock(path):
    path = _plain(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as stream:
        if stream.tell() == 0:
            stream.write(b"0")
            stream.flush()
        stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError("Another Texture Swapper operation is active") from error
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def ensure_game_closed():
    """Fail closed if Windows process enumeration cannot be verified."""
    guarded = {"everquest2.exe", "eq2.exe", "launchpad.exe", "eq2patch.exe", "eq2patcher.exe"}
    if os.name == "nt":
        from texturequest_processes import ensure_closed
        return ensure_closed()
    else:
        import psutil
        active = [p.info["name"] for p in psutil.process_iter(["name"]) if (p.info["name"] or "").casefold() in guarded]
    if active:
        raise RuntimeError("Close EverQuest II and its patcher first: " + ", ".join(active))


@contextmanager
def _game_lock(paks):
    """Serialize different portable app copies targeting the same game folder."""
    paks = _plain(paks)
    if os.name != "nt":
        import fcntl
        fd = os.open(paks, os.O_RDONLY)
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                raise RuntimeError("Another Texture Swapper is changing this game installation") from error
            yield
        finally:
            os.close(fd)
        return
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateMutexW.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.ReleaseMutex.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    name = "Local\\EQ2TextureSwapper-" + hashlib.sha256(os.path.normcase(str(paks)).encode()).hexdigest()
    handle = kernel.CreateMutexW(None, False, name)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    acquired = False
    try:
        result = kernel.WaitForSingleObject(handle, 0)
        acquired = result in (0, 0x80)  # abandoned lock is recoverable from journal
        if not acquired:
            if result == 0xFFFFFFFF:
                raise ctypes.WinError(ctypes.get_last_error())
            raise RuntimeError("Another Texture Swapper is changing this game installation")
        yield
    finally:
        if acquired:
            kernel.ReleaseMutex(handle)
        kernel.CloseHandle(handle)


def _zip_bounds(path):
    """Bound the central directory before zipfile allocates its member list."""
    size = path.stat().st_size
    if not 22 <= size <= MAX_TOTAL_BYTES + MAX_INDEX_BYTES:
        raise ValueError("ZIP file is empty or exceeds supported size")
    with path.open("rb") as stream:
        stream.seek(max(0, size - 65557))
        tail = stream.read(65557)
        pos = tail.rfind(b"PK\x05\x06")
        if pos < 0 or pos + 22 > len(tail):
            raise ValueError("ZIP end record is missing")
        disk, directory_disk, disk_entries, count, central_size, central_offset, comment = struct.unpack_from("<HHHHIIH", tail, pos + 4)
        eocd = size - len(tail) + pos
        if pos + 22 + comment != len(tail) or disk or directory_disk or disk_entries != count:
            raise ValueError("Multipart or trailing-data ZIP is unsupported")
        if count == 0xFFFF or central_size == 0xFFFFFFFF or central_offset == 0xFFFFFFFF:
            if eocd < 20:
                raise ValueError("ZIP64 locator is missing")
            stream.seek(eocd - 20)
            signature, location_disk, location, disks = struct.unpack("<IIQI", stream.read(20))
            if signature != 0x07064B50 or location_disk or disks != 1 or location + 56 > eocd - 20:
                raise ValueError("Invalid ZIP64 locator")
            stream.seek(location)
            data = stream.read(56)
            signature, record_size, made, needed, disk, directory_disk, disk_entries, count, central_size, central_offset = struct.unpack("<IQHHIIQQQQ", data)
            if signature != 0x06064B50 or record_size < 44 or record_size > CHUNK or disk or directory_disk or disk_entries != count:
                raise ValueError("Invalid ZIP64 directory")
        if not 1 <= count <= MAX_ARCHIVES * 3 + 1 or not 1 <= central_size <= MAX_INDEX_BYTES or central_offset + central_size > eocd:
            raise ValueError("ZIP directory exceeds its bounds")


def validate_game(game_dir):
    path = _plain(game_dir)
    root = path.parent if path.name.casefold() == "paks" else path
    paks = path if path.name.casefold() == "paks" else path / "paks"
    if not root.is_dir() or not _plain(paks).is_dir():
        raise ValueError("Select the EverQuest II folder containing its paks directory")
    exe = next((p for p in root.iterdir() if p.name.casefold() == "everquest2.exe"), None)
    if exe is None or not _plain(exe).is_file():
        raise ValueError("The selected folder does not contain EverQuest2.exe")
    return paks


class Swapper:
    def __init__(self, state_dir):
        self.state_dir = _plain(state_dir)
        self.state_dir.mkdir(parents=True, exist_ok=True)

    validate_game = staticmethod(validate_game)

    def _game_manifest(self, game_dir):
        paks = validate_game(game_dir)
        if self.state_dir == paks.parent or self.state_dir.is_relative_to(paks.parent):
            raise ValueError("Swapper data must be stored outside the game installation")
        key = hashlib.sha256(os.path.normcase(str(paks)).encode()).hexdigest()
        path = _plain(self.state_dir / "games" / key / "manifest.json", self.state_dir)
        return paks, path

    def _prepared(self, value):
        if not isinstance(value, dict) or not re.fullmatch(r"[0-9a-f]{32}", str(value.get("id", ""))):
            raise ValueError("Choose and inspect a texture pack first")
        path = _plain(self.state_dir / "prepared" / value["id"] / "manifest.json", self.state_dir)
        saved = _load(path)
        if saved != value or saved.get("schema") != SCHEMA + "-prepared":
            raise ValueError("Prepared pack changed; choose it again")
        if not 1 <= len(saved.get("archives", [])) <= MAX_ARCHIVES:
            raise ValueError("Invalid prepared archive inventory")
        for row in saved["archives"]:
            _name(row["name"])
            expected = path.parent / row["name"]
            if _plain(row["path"], path.parent) != expected:
                raise ValueError("Prepared archive path changed")
        return saved

    def prepare(self, paths, progress=None):
        notify = progress or (lambda message: None)
        paths = list(paths)
        if not 1 <= len(paths) <= MAX_ARCHIVES:
            raise ValueError("Select a ZIP or one or more EQ2 .vpk/.pak files")
        with _lock(self.state_dir / "operation.lock"):
            identity = uuid.uuid4().hex
            folder = _plain(self.state_dir / "prepared" / identity, self.state_dir)
            folder.mkdir(parents=True)
            records, names, total = [], set(), 0

            def add(name, reader, size, expected=None):
                nonlocal total
                _name(name)
                if name.casefold() in names or not 16 <= size <= MAX_ARCHIVE_BYTES or len(records) >= MAX_ARCHIVES:
                    raise ValueError("Duplicate, excessive or oversized archive selection")
                total += size
                if total > MAX_TOTAL_BYTES:
                    raise ValueError("Texture pack exceeds supported size")
                names.add(name.casefold())
                target = _plain(folder / name, folder)
                if shutil.disk_usage(folder).free < size + 64 * CHUNK:
                    raise OSError("Not enough free space to inspect this pack")
                notify("Checking " + name)
                digest, written = hashlib.sha256(), 0
                with target.open("xb") as output:
                    while chunk := reader.read(CHUNK):
                        written += len(chunk)
                        if written > size:
                            raise ValueError("Archive exceeds its advertised size")
                        output.write(chunk)
                        digest.update(chunk)
                    output.flush()
                    os.fsync(output.fileno())
                if written != size:
                    raise ValueError("Archive size changed while reading")
                entries = validate_archive(target)
                record = dict(name=name, path=str(target), sha256=digest.hexdigest(), bytes=written, entries=len(entries))
                if expected and any(record[k] != expected[k] for k in ("name", "sha256", "bytes", "entries")):
                    raise ValueError("Texture Studio manifest does not match the archive")
                records.append(record)

            for source in paths:
                source = _plain(source)
                if not source.is_file():
                    raise ValueError("Selected pack does not exist")
                if source.suffix.casefold() != ".zip":
                    with source.open("rb") as stream:
                        add(source.name, stream, source.stat().st_size)
                    continue
                _zip_bounds(source)
                with zipfile.ZipFile(source) as archive:
                    infos = archive.infolist()
                    if not 1 <= len(infos) <= MAX_ARCHIVES * 3 + 1:
                        raise ValueError("ZIP contains too many members")
                    seen, members, manifests, expanded = set(), [], [], 0
                    for item in infos:
                        _member(item.filename, allow_directory=item.is_dir())
                        folded = item.filename.rstrip("/").casefold()
                        mode = (item.external_attr >> 16) & 0xFFFF
                        if (folded in seen or item.flag_bits & 1 or stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)
                                or item.external_attr & 0x400 or item.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)):
                            raise ValueError("Unsafe, duplicate, encrypted or linked ZIP member")
                        seen.add(folded)
                        if item.is_dir():
                            if item.file_size:
                                raise ValueError("ZIP directory contains data")
                            continue
                        if stat.S_IFMT(mode) == stat.S_IFDIR or not 0 <= item.file_size <= MAX_ARCHIVE_BYTES:
                            raise ValueError("Invalid ZIP file metadata")
                        expanded += item.file_size
                        if expanded > MAX_TOTAL_BYTES or item.file_size > max(1, item.compress_size) * 200:
                            raise ValueError("ZIP expanded size or compression ratio is excessive")
                        if item.filename == "manifest.json":
                            if item.file_size > MAX_MANIFEST_BYTES:
                                raise ValueError("ZIP manifest is too large")
                            manifests.append(item)
                        elif Path(item.filename).suffix.casefold() in (".vpk", ".pak"):
                            members.append(item)
                        else:
                            raise ValueError("ZIP must contain only EQ2 archives and an optional Texture Studio manifest")
                    expected = {}
                    if manifests:
                        value = _json(archive.read(manifests[0]))
                        if (value.get("schema") != "eq2-texture-pack-v1" or value.get("game") != "EverQuest II Origins"
                                or value.get("pack_id") != _digest({k: v for k, v in value.items() if k != "pack_id"})):
                            raise ValueError("Texture Studio manifest checksum or format changed")
                        rows = value.get("archives", [])
                        if not isinstance(rows, list) or not 1 <= len(rows) <= MAX_ARCHIVES:
                            raise ValueError("Invalid Texture Studio archive inventory")
                        for row in rows:
                            _name(row["name"])
                            if row.get("member") != "paks/" + row["name"] or row["member"] in expected:
                                raise ValueError("Duplicate or unsafe Texture Studio archive inventory")
                            expected[row["member"]] = row
                        if set(expected) != {item.filename for item in members}:
                            raise ValueError("Texture Studio manifest lists missing or unexpected archives")
                    if not members:
                        raise ValueError("ZIP contains no EQ2 archives")
                    for item in members:
                        with archive.open(item) as stream:
                            add(Path(item.filename).name, stream, item.file_size, expected.get(item.filename))
            value = dict(schema=SCHEMA + "-prepared", id=identity, created_at=_now(),
                         archives=records, total_bytes=total, source_paths=[str(_plain(p)) for p in paths])
            _save(folder / "manifest.json", value)
            notify(f"Ready: {len(records)} archive(s)")
            return value

    def preflight(self, prepared, game_dir):
        value = self._prepared(prepared)
        paks, _ = self._game_manifest(game_dir)
        existing = {}
        for path in paks.iterdir():
            if path.suffix.casefold() in (".vpk", ".pak"):
                if path.name.casefold() in existing:
                    raise ValueError("Game contains ambiguous archive filenames")
                existing[path.name.casefold()] = path
        rows, missing = [], []
        for row in value["archives"]:
            target = existing.get(row["name"].casefold())
            if target is None:
                missing.append(row["name"])
                continue
            if not _plain(target, paks).is_file():
                raise ValueError("Destination archive is not a regular file")
            rows.append(dict(row, target=str(target), target_name=target.name))
        return dict(game_dir=str(paks.parent), paks_dir=str(paks), archives=rows,
                    missing=missing, total_bytes=value["total_bytes"])

    def _manifest(self, paks, path):
        if not path.exists():
            return dict(schema=SCHEMA, game_dir=str(paks.parent), paks_dir=str(paks), status="inactive", archives=[], created_at=_now())
        value = _load(path)
        if value.get("schema") != SCHEMA or value.get("paks_dir") != str(paks):
            raise ValueError("Swapper manifest belongs to a different installation")
        names = set()
        for row in value.get("archives", []):
            _name(row["name"])
            if row["name"].casefold() in names:
                raise ValueError("Duplicate managed archive name")
            names.add(row["name"].casefold())
        return value

    def _clear_staged(self, paks, record):
        """Remove only a journaled sibling copy, never an installed archive."""
        name = record.get("staged_name")
        if not name:
            return
        pattern = re.escape("." + record["name"] + ".swapper-") + r"[0-9a-f]{32}\.tmp"
        if not isinstance(name, str) or not re.fullmatch(pattern, name):
            raise ValueError("Invalid staged file in the recovery manifest")
        staged = _plain(paks / name, paks)
        if staged.exists():
            # Interrupted copies may be short; the unguessable, durable name is
            # owned by this operation. Never follow a replaced filesystem link.
            if not staged.is_file():
                raise ValueError("Staged copy was replaced by a directory")
            if _readonly(staged):
                _set_readonly(staged, False)
            staged.unlink()

    def install(self, prepared, game_dir, progress=None):
        notify = progress or (lambda message: None)
        with _lock(self.state_dir / "operation.lock"), _game_lock(validate_game(game_dir)):
            ready = self.preflight(prepared, game_dir)
            if ready["missing"]:
                raise ValueError("These archives are not installed in the selected game: " + ", ".join(ready["missing"]))
            paks, path = self._game_manifest(game_dir)
            ensure_game_closed()
            for row in ready["archives"]:
                if _plain(row["path"]).stat().st_size != row["bytes"] or _hash(row["path"]) != row["sha256"]:
                    raise ValueError("Prepared archive changed; inspect the pack again")
                validate_archive(row["path"])
            value = self._manifest(paks, path)
            for old_record in value["archives"]:
                self._clear_staged(paks, old_record)
            attempt = uuid.uuid4().hex
            value.update(status="installing", operation="install", attempt_id=attempt, pack_id=prepared["id"], error=None, message=None)
            managed = {row["name"].casefold(): row for row in value["archives"]}
            # Commit ownership before touching bytes or attributes; interruption is recoverable.
            for row in ready["archives"]:
                target = _plain(row["target"], paks)
                old = managed.get(target.name.casefold(), {})
                previous_hash = _hash(target)
                record = dict(name=target.name, sha256=row["sha256"], bytes=row["bytes"], managed=True,
                              status="pending", previous_sha256=previous_hash, previous_readonly=_readonly(target),
                              original_sha256=old.get("original_sha256", previous_hash), selected_at=_now(),
                              staged_name=f".{target.name}.swapper-{attempt}.tmp")
                managed[target.name.casefold()] = record
            value["archives"] = list(managed.values())
            _save(path, value)
            try:
                for row in ready["archives"]:
                    ensure_game_closed()
                    target = _plain(row["target"], paks)
                    record = managed[target.name.casefold()]
                    staged = _plain(paks / record["staged_name"], paks)
                    notify("Installing " + target.name)
                    if _hash(target) != row["sha256"]:
                        if shutil.disk_usage(paks).free < row["bytes"] + 16 * CHUNK:
                            raise OSError("Not enough space in the game folder to stage this archive")
                        with _plain(row["path"]).open("rb") as source, staged.open("xb") as output:
                            shutil.copyfileobj(source, output, CHUNK)
                            output.flush()
                            os.fsync(output.fileno())
                        if staged.stat().st_size != row["bytes"] or _hash(staged) != row["sha256"]:
                            raise ValueError("Staged archive did not verify")
                        record["status"] = "staged"
                        _save(path, value)
                        ensure_game_closed()
                        _set_readonly(target, False)
                        os.replace(staged, target)
                    # Clear flags left by an older installer, never protect files
                    # against the official patcher. New staged files are writable.
                    if _readonly(target):
                        _set_readonly(target, False)
                    if _hash(target) != row["sha256"]:
                        raise ValueError("Installed archive failed final verification")
                    record.update(status="installed", installed_at=_now(), readonly=False)
                    _save(path, value)
                value.update(status="installed", completed_at=_now())
                _save(path, value)
                _save(path.parent / "attempts" / (attempt + ".json"), dict(value))
            except BaseException as error:
                value.update(status="interrupted", error=str(error))
                _save(path, value)
                _save(path.parent / "attempts" / (attempt + ".json"), dict(value))
                raise
            notify("Installed and verified. Rerun TextureQuest after a game patch.")
            return self.status(game_dir)

    def uninstall(self, game_dir, progress=None):
        notify = progress or (lambda message: None)
        with _lock(self.state_dir / "operation.lock"), _game_lock(validate_game(game_dir)):
            paks, path = self._game_manifest(game_dir)
            value = self._manifest(paks, path)
            if not any(row.get("managed") for row in value["archives"]):
                return self.status(game_dir)
            ensure_game_closed()
            attempt = uuid.uuid4().hex
            value.update(status="uninstalling", operation="uninstall", attempt_id=attempt, error=None)
            _save(path, value)
            try:
                for row in value["archives"]:
                    if not row.get("managed"):
                        continue
                    ensure_game_closed()
                    target = _plain(paks / row["name"], paks)
                    self._clear_staged(paks, row)
                    notify("Unlocking " + row["name"])
                    if target.exists():
                        if not target.is_file():
                            raise ValueError("Managed archive was replaced by a directory")
                        current = _hash(target)
                        row.update(uninstall_sha256=current, changed_externally=current != row["sha256"])
                        _set_readonly(target, False)
                    row.update(managed=False, status="unlocked" if target.exists() else "missing", readonly=False, uninstalled_at=_now())
                    _save(path, value)
                value.update(status="inactive", completed_at=_now(), message="Run the official EQ2 patcher to restore the original game files.")
                _save(path, value)
                _save(path.parent / "attempts" / (attempt + ".json"), dict(value))
            except BaseException as error:
                value.update(status="interrupted", error=str(error))
                _save(path, value)
                _save(path.parent / "attempts" / (attempt + ".json"), dict(value))
                raise
            notify(value["message"])
            return self.status(game_dir)

    def status(self, game_dir):
        paks, path = self._game_manifest(game_dir)
        value = self._manifest(paks, path)
        rows = [_installed_integrity(paks / row["name"], row) for row in value["archives"]]
        managed = [row for row in rows if row.get("managed")]
        counts = {name + "_count": sum(row["integrity"] == name for row in managed)
                  for name in ("intact", "changed", "missing", "unverified")}
        counts["unprotected_count"] = sum(row["exists"] is True and row["readonly"] is False for row in managed)
        readonly = sum(row["readonly"] is True for row in managed)
        issues = [f"{counts[key]} {description}" for key, description in (
            ("changed_count", "changed from the installed pack"),
            ("missing_count", "missing"),
            ("unverified_count", "could not be verified")) if counts[key]]
        recorded_status = value["status"]
        operation_incomplete = recorded_status in ("installing", "uninstalling", "interrupted", "error")
        status = recorded_status if operation_incomplete or not issues else "needs_attention"
        if managed:
            message = ("Archive verification: " + "; ".join(issues) + ".") if issues else (
                f"All {len(managed)} managed archives match the installed pack.")
            if operation_incomplete:
                reason = value.get("error") or f"The last {value.get('operation', 'operation')} did not complete."
                message = str(reason) + " " + message
        else:
            message = value.get("message") or value.get("error") or "No active texture pack."
        return dict(game_dir=str(paks.parent), paks_dir=str(paks), managed_count=len(managed),
                    readonly_count=readonly, status=status, recorded_status=recorded_status,
                    message=message, **counts,
                    archives=rows, manifest_path=str(path))
