"""Verified portable-app updates, staged outside the application being replaced.

Only MANAGED roots move. The portable ``app_dir/data`` tree and every unknown
application-root entry are never enumerated, copied or removed. A helper READY
receipt precedes closing the old GUI; a view-ready startup ACK precedes commit.

``stage_downloaded_update`` is the offline entry point for an already downloaded
official installer, including isolated onedir fixtures. It applies the SAME ZIP,
checksum, layout and parent checks as ``stage_update``. ``processes`` injection
is a small dependency interface for filesystem tests, not an update-policy bypass.
Production uses WindowsProcesses. ``smoke_test`` propagates only the existing
fixed --smoke-test argument; no arbitrary restart arguments are accepted.
"""
from __future__ import annotations

from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import time
import uuid
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import zipfile

import texturequest_latest as latest
import texturequest_version_check as versions

MANAGED = ("TextureQuest.exe", "_internal", "licenses", "START-HERE.txt", "BUILD-MANIFEST.json")
DIRECTORIES = {"_internal", "licenses"}
MAX_ZIP = 512 * 1024**2
MAX_EXPANDED = 1024**3
MAX_FILE = 256 * 1024**2
MAX_JSON = 4 * 1024**2
MAX_FILES = 20000
READY_SECONDS, PARENT_SECONDS, ACK_SECONDS = 20, 120, 60
REQUEST_SCHEMA = "texturequest-app-update-request-v1"
_HEX = re.compile(r"[0-9a-f]{64}")
_NONCE = re.compile(r"[0-9a-f]{32}")
_RESERVED = re.compile(r"(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", re.I)


class AppUpdateError(RuntimeError):
    pass


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                     separators=(",", ":")).encode("utf-8")).hexdigest()


def _seal(value):
    value = dict(value)
    value["record_sha256"] = _digest(value)
    return value


def _pairs(rows):
    value = {}
    for key, item in rows:
        if key in value:
            raise AppUpdateError("Duplicate JSON key")
        value[key] = item
    return value


def _json(raw):
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs)
    except (ValueError, UnicodeError) as error:
        raise AppUpdateError("Invalid update JSON") from error


def _plain(path):
    """Reject redirected ancestors without resolving through them."""
    path = Path(os.path.abspath(path))
    for item in reversed((path, *path.parents)):
        if not item.exists() and not item.is_symlink():
            continue
        value = item.lstat()
        if stat.S_ISLNK(value.st_mode) or getattr(value, "st_file_attributes", 0) & 0x400:
            raise AppUpdateError("Update paths cannot contain symbolic links or reparse points")
    return path


def _same_path(left, right):
    return os.path.normcase(os.path.abspath(left)) == os.path.normcase(os.path.abspath(right))


def _identity(path):
    path = _plain(path)
    value = path.stat()
    return [value.st_dev, value.st_ino]


@contextmanager
def _reader(path):
    """Close a Windows deny-write/delete handle before any rename or process start."""
    path = _plain(path)
    if os.name != "nt":
        with path.open("rb") as stream:
            yield stream
        return
    import msvcrt
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                  wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x00200000 | 0x08000000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "Cannot acquire protected app-update file", str(path))
    try:
        descriptor = msvcrt.open_osfhandle(int(handle), os.O_RDONLY | os.O_BINARY)
    except Exception:
        kernel.CloseHandle(handle)
        raise
    with os.fdopen(descriptor, "rb") as stream:
        value = os.fstat(stream.fileno())
        if (not stat.S_ISREG(value.st_mode) or value.st_nlink != 1
                or getattr(value, "st_file_attributes", 0) & 0x400):
            raise AppUpdateError("Update handle is not an independent ordinary file")
        yield stream


def _file(path, maximum=MAX_FILE):
    path = _plain(path)
    before = path.stat()
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or not 0 <= before.st_size <= maximum:
        raise AppUpdateError("Update file is not a bounded, independent regular file: " + str(path))
    # Python 3.12 Windows path.stat's deprecated ctime reports creation time,
    # whereas fstat can report change time. Use the explicit birthtime there.
    stamp = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, getattr(s, "st_birthtime_ns", s.st_ctime_ns))
    digest = hashlib.sha256()
    with _reader(path) as stream:
        if stamp(os.fstat(stream.fileno())) != stamp(before):
            raise AppUpdateError("Update file changed while opening")
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        if stamp(os.fstat(stream.fileno())) != stamp(before):
            raise AppUpdateError("Update file changed while reading")
    if stamp(path.stat()) != stamp(before):
        raise AppUpdateError("Update file changed after reading")
    return {"bytes": before.st_size, "sha256": digest.hexdigest()}


def _read(path):
    pin = _file(path, MAX_JSON)
    with _reader(path) as stream:
        raw = stream.read(MAX_JSON + 1)
    if len(raw) != pin["bytes"] or hashlib.sha256(raw).hexdigest() != pin["sha256"]:
        raise AppUpdateError("Update metadata changed while loading")
    return _json(raw)


def _load(path):
    value = _read(path)
    if not isinstance(value, dict):
        raise AppUpdateError("Invalid update record")
    body = {k: v for k, v in value.items() if k != "record_sha256"}
    if value.get("record_sha256") != _digest(body):
        raise AppUpdateError("Update record checksum mismatch")
    return value


def _write(path, value):
    path = _plain(path)
    if not path.parent.is_dir():
        raise AppUpdateError("Update record parent is unavailable")
    raw = (json.dumps(_seal(value), indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    return _file(path, MAX_JSON)


def _relative(name):
    if (not isinstance(name, str) or not name or "\\" in name or name.startswith("/")
            or len(name) > 220 or any(ord(c) < 32 for c in name)):
        raise AppUpdateError("Unsafe installer member path")
    parts = name.split("/")
    if any(p in ("", ".", "..") or p[-1:] in (".", " ") or ":" in p
           or _RESERVED.fullmatch(p) or any(c in p for c in '<>"|?*') for p in parts):
        raise AppUpdateError("Unsafe Windows installer member path")
    if parts[0] not in MANAGED or (len(parts) > 1 and parts[0] not in DIRECTORIES):
        raise AppUpdateError("Installer contains an unmanaged application path")
    return PurePosixPath(name)


def _manifest(value, version):
    if not isinstance(value, dict) or value.get("application") != "TextureQuest" or value.get("version") != version:
        raise AppUpdateError("Installer application/version mismatch")
    for key in ("version", "texture_version", "manifest_version"):
        latest.version_number(value.get(key))
    if not _HEX.fullmatch(str(value.get("release_manifest_sha256", ""))):
        raise AppUpdateError("Missing bundled release manifest checksum")
    sources = value.get("source_sha256")
    if not isinstance(sources, dict) or not sources or not all(_HEX.fullmatch(str(v)) for v in sources.values()):
        raise AppUpdateError("Missing build source inventory")
    rows = value.get("files")
    if not isinstance(rows, list) or not 4 <= len(rows) <= MAX_FILES:
        raise AppUpdateError("Invalid build inventory length")
    result, seen, total = {}, set(), 0
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"path", "bytes", "sha256"}:
            raise AppUpdateError("Invalid build inventory row")
        name = str(_relative(row["path"]))
        if (name == "BUILD-MANIFEST.json" or name.casefold() in seen
                or type(row["bytes"]) is not int or not 0 <= row["bytes"] <= MAX_FILE
                or not _HEX.fullmatch(str(row["sha256"]))):
            raise AppUpdateError("Invalid or duplicated build file identity")
        result[name] = {"bytes": row["bytes"], "sha256": row["sha256"]}
        seen.add(name.casefold())
        total += row["bytes"]
    if total > MAX_EXPANDED or "TextureQuest.exe" not in result or result["TextureQuest.exe"]["bytes"] < 64:
        raise AppUpdateError("Invalid installer size or executable")
    if (not any(p.startswith("_internal/") for p in result)
            or not any(p.startswith("licenses/") for p in result) or "START-HERE.txt" not in result):
        raise AppUpdateError("Installer is missing managed application roots")
    return result


def _bundle(root, version):
    """Inventory only the managed trees. Never enumerate root/data/unknown roots."""
    root = _plain(root)
    manifest = _read(root / "BUILD-MANIFEST.json")
    expected = _manifest(manifest, version)
    actual = set()
    for name in MANAGED:
        path = _plain(root / name)
        if name in DIRECTORIES:
            if not path.is_dir():
                raise AppUpdateError("Missing application directory: " + name)
            for folder, dirs, files in os.walk(path, followlinks=False):
                for entry in dirs:
                    if not _plain(Path(folder) / entry).is_dir():
                        raise AppUpdateError("Unsafe application directory")
                for entry in files:
                    actual.add((Path(folder) / entry).relative_to(root).as_posix())
        elif not path.is_file():
            raise AppUpdateError("Missing application file: " + name)
        else:
            actual.add(name)
    if actual != set(expected) | {"BUILD-MANIFEST.json"}:
        raise AppUpdateError("Managed application directories contain unlisted or missing files")
    for name, pin in expected.items():
        if _file(root / name) != pin:
            raise AppUpdateError("Application file checksum mismatch: " + name)
    with (root / "TextureQuest.exe").open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise AppUpdateError("Installer executable is not a Windows executable")
    bundled = root / "_internal/release-manifest.json"
    if _file(bundled, MAX_JSON)["sha256"] != manifest["release_manifest_sha256"]:
        raise AppUpdateError("Bundled release manifest checksum mismatch")
    release = _read(bundled)
    if (release.get("version") != manifest["manifest_version"]
            or versions.texture_version(release) != manifest["texture_version"]):
        raise AppUpdateError("Bundled texture/release versions disagree")
    return {"manifest": _file(root / "BUILD-MANIFEST.json", MAX_JSON), "files": expected}


def _installer(value, current_version):
    if not isinstance(value, dict) or value.get("status") != "verified":
        raise AppUpdateError("No verified official installer is available")
    version = value.get("version")
    if latest.version_number(version) <= latest.version_number(current_version):
        raise AppUpdateError("Installer must be newer than the running app")
    page = versions.installer_page(value)
    tag = page.rsplit("/", 1)[1]
    expected = latest.REPOSITORY + "/releases/download/" + tag + f"/TextureQuest-Installer-{version}-windows-x64.zip"
    if (value.get("download_url") != expected or type(value.get("bytes")) is not int
            or not 0 < value["bytes"] <= MAX_ZIP or not _HEX.fullmatch(str(value.get("sha256", "")))):
        raise AppUpdateError("Invalid official installer download identity")
    return {k: value[k] for k in ("status", "version", "download_url", "release_url", "bytes", "sha256")}


def _layout(app_dir, data_dir):
    app, data = _plain(app_dir), _plain(data_dir)
    if not app.is_dir() or app.parent == app or len(app.parts) < 3:
        raise AppUpdateError("Choose a portable application folder, not a filesystem root")
    if not _same_path(data, app / "data"):
        raise AppUpdateError("Automatic app updates require the portable app's data folder. Custom --data-dir is preserved by refusing this update.")
    if data.exists() and not data.is_dir():
        raise AppUpdateError("Portable data path is not a directory")
    return app, data


def _progress(report, phase, message, done=0, total=0):
    if report:
        report({"phase": phase, "message": message, "done": done, "total": total})


def _download(installer, destination, opener, report):
    destination = _plain(destination)
    owned, complete = None, False
    url = installer["download_url"]
    request = Request(url, headers={"User-Agent": "TextureQuest-App-Updater", "Accept": "application/octet-stream"})
    try:
        with opener(request, timeout=30) as response:
            final = urlsplit(response.geturl())
            if (final.scheme != "https" or final.hostname not in {"github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}
                    or final.username or final.password or final.port not in (None, 443) or response.status != 200):
                raise AppUpdateError("Installer download returned an unexpected endpoint")
            length = response.headers.get("Content-Length")
            if length is not None and (not length.isdecimal() or int(length) != installer["bytes"]):
                raise AppUpdateError("Installer download size changed")
            done, digest = 0, hashlib.sha256()
            with destination.open("xb") as stream:
                owned = _identity(destination)
                while True:
                    block = response.read(min(1024 * 1024, installer["bytes"] - done + 1))
                    if not block:
                        break
                    done += len(block)
                    if done > installer["bytes"]:
                        raise AppUpdateError("Installer download exceeds its verified size")
                    digest.update(block)
                    stream.write(block)
                    _progress(report, "download", "Downloading the verified app update", done, installer["bytes"])
                stream.flush()
                os.fsync(stream.fileno())
        if done != installer["bytes"] or digest.hexdigest() != installer["sha256"]:
            raise AppUpdateError("Installer download failed checksum verification")
        pin = _file(destination, MAX_ZIP)
        if pin != {"bytes": installer["bytes"], "sha256": installer["sha256"]}:
            raise AppUpdateError("Installer file changed after download")
        complete = True
        return pin
    finally:
        if not complete and owned is not None and _identity(destination) == owned:
            _plain(destination).unlink()


def _extract(archive, destination, version, archive_pin=None):
    destination = _plain(destination)
    destination.mkdir()
    with _reader(archive) as retained:
        if archive_pin is not None:
            digest, size = hashlib.sha256(), 0
            for block in iter(lambda: retained.read(1024 * 1024), b""):
                size += len(block)
                if size > MAX_ZIP:
                    raise AppUpdateError("Installer exceeds its download bound")
                digest.update(block)
            if {"bytes": size, "sha256": digest.hexdigest()} != archive_pin:
                raise AppUpdateError("Installer changed before protected extraction")
            retained.seek(0)
        return _extract_retained(retained, destination, version)


def _extract_retained(retained, destination, version):
    with zipfile.ZipFile(retained) as source:
        entries, seen, expanded = {}, set(), 0
        for info in source.infolist():
            if not info.filename.startswith("TextureQuest/"):
                raise AppUpdateError("Installer ZIP has an unexpected root")
            name = info.filename[len("TextureQuest/"):]
            _relative(name)
            mode = info.external_attr >> 16
            if (info.is_dir() or name.casefold() in seen or info.flag_bits & 1
                    or info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
                    or (mode and stat.S_IFMT(mode) not in (0, stat.S_IFREG))
                    or info.external_attr & 0x400 or not 0 <= info.file_size <= MAX_FILE):
                raise AppUpdateError("Installer ZIP contains an unsafe member")
            expanded += info.file_size
            entries[name], _ = info, seen.add(name.casefold())
            if len(entries) > MAX_FILES + 1 or expanded > MAX_EXPANDED:
                raise AppUpdateError("Installer ZIP exceeds extraction limits")
        if "BUILD-MANIFEST.json" not in entries or entries["BUILD-MANIFEST.json"].file_size > MAX_JSON:
            raise AppUpdateError("Installer has no bounded build manifest")
        manifest = _json(source.read(entries["BUILD-MANIFEST.json"]))
        expected = _manifest(manifest, version)
        if set(entries) != set(expected) | {"BUILD-MANIFEST.json"}:
            raise AppUpdateError("ZIP contents differ from the build inventory")
        for name, info in entries.items():
            path = _plain(destination / name)
            path.parent.mkdir(parents=True, exist_ok=True)
            digest, done = hashlib.sha256(), 0
            with source.open(info) as stream, path.open("xb") as output:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    done += len(block)
                    if done > info.file_size:
                        raise AppUpdateError("ZIP member exceeds its declared size")
                    output.write(block)
                    digest.update(block)
            if done != info.file_size or (name in expected and {"bytes": done, "sha256": digest.hexdigest()} != expected[name]):
                raise AppUpdateError("ZIP member checksum mismatch")
    return _bundle(destination, version)


def process_identity(pid=None):
    """Exact native process identity; a leaf that isolated fixtures may replace."""
    import psutil
    process = psutil.Process(os.getpid() if pid is None else pid)
    return {"pid": process.pid, "birth": process.create_time(), "exe": os.path.abspath(process.exe())}


def _valid_process(value):
    if (not isinstance(value, dict) or set(value) != {"pid", "birth", "exe"}
            or type(value["pid"]) is not int or value["pid"] <= 0
            or not isinstance(value["birth"], (float, int)) or isinstance(value["birth"], bool)
            or value["birth"] <= 0 or not isinstance(value["exe"], str) or not Path(value["exe"]).is_absolute()):
        raise AppUpdateError("Invalid process identity")
    return value


def _same_process(actual, expected):
    return (actual["pid"] == expected["pid"] and actual["birth"] == expected["birth"]
            and _same_path(actual["exe"], expected["exe"]))


class WindowsProcesses:
    """Exact PID/birth/executable checks and a retained handle for parent exit."""
    def identity(self, pid=None):
        return process_identity(pid)

    def alive(self, identity):
        import psutil
        try:
            return _same_process(process_identity(identity["pid"]), identity)
        except psutil.NoSuchProcess:
            return False

    def other_instances(self, app, allowed=()):
        import psutil
        allowed = [*allowed, self.identity()]
        for process in psutil.process_iter(["pid", "name"]):
            if (process.info.get("name") or "").casefold() != "texturequest.exe":
                continue
            try:
                identity = process_identity(process.pid)
            except psutil.NoSuchProcess:
                continue
            except psutil.AccessDenied as error:
                raise AppUpdateError("Cannot establish another TextureQuest process identity") from error
            if not any(_same_process(identity, x) for x in allowed):
                raise AppUpdateError("Close other TextureQuest app instances before updating")

    def spawn(self, executable, arguments, cwd):
        import psutil
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        # Only the staging helper is hidden. The restarted GUI's first native
        # ShowWindow call must inherit SW_SHOWNORMAL, including rollback restart.
        startup.wShowWindow = 0 if arguments[0] == "--apply-app-update" else 1
        process = subprocess.Popen([str(executable), *arguments], cwd=str(cwd), startupinfo=startup,
                                   creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            identity = process_identity(process.pid)
        except psutil.NoSuchProcess as error:
            raise AppUpdateError("The app update helper exited during launch") from error
        if not _same_path(identity["exe"], executable):
            raise AppUpdateError("Unexpected executable after launching app update")
        return identity

    @contextmanager
    def parent(self, identity):
        if os.name != "nt":
            raise AppUpdateError("Automatic application replacement requires Windows")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
                                          ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME)]
        kernel.GetProcessTimes.restype = wintypes.BOOL
        kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        handle = kernel.OpenProcess(0x100000 | 0x1000, False, identity["pid"])
        if not handle:
            raise AppUpdateError("The original app process is no longer available")
        try:
            times = [wintypes.FILETIME() for _ in range(4)]
            buffer, length = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
            if (not kernel.GetProcessTimes(handle, *(ctypes.byref(x) for x in times))
                    or not kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(length))):
                raise AppUpdateError("Cannot validate the original app process handle")
            birth = ((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime) / 10000000 - 11644473600
            if abs(birth - identity["birth"]) > 0.000002 or not _same_path(buffer.value, identity["exe"]) or not self.alive(identity):
                raise AppUpdateError("Original app process identity changed")

            def wait(seconds):
                result = kernel.WaitForSingleObject(handle, round(seconds * 1000))
                if result not in (0, 258):
                    raise AppUpdateError("Cannot wait for the original app to close")
                return result == 0
            yield wait
        finally:
            kernel.CloseHandle(handle)

    def stop_owned(self, child, helper):
        """Only stop the exact child we started, after a failed startup ACK."""
        import psutil
        if not self.alive(child):
            return
        process = psutil.Process(child["pid"])
        if process.ppid() != helper["pid"] or not self.alive(helper):
            raise AppUpdateError("Will not stop a process with changed app-update ownership")
        process.terminate()
        process.wait(timeout=15)

    def alert(self, message):
        user = ctypes.WinDLL("user32", use_last_error=True)
        user.MessageBoxW.argtypes = [wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.UINT]
        user.MessageBoxW.restype = ctypes.c_int
        user.MessageBoxW(None, message, "TextureQuest app update", 0x10)


def _request(path, nonce):
    if not isinstance(nonce, str) or not _NONCE.fullmatch(nonce):
        raise AppUpdateError("Invalid app-update nonce")
    path = _plain(path)
    value = _load(path)
    required = {"schema", "nonce", "app_dir", "data_dir", "app_identity", "stage_identity", "current_version", "target_version",
                "installer", "parent", "old_bundle", "new_bundle", "record_sha256"}
    if set(value) != required or value["schema"] != REQUEST_SCHEMA or value["nonce"] != nonce:
        raise AppUpdateError("App-update request identity mismatch")
    app, data = _layout(value["app_dir"], value["data_dir"])
    stage = _plain(app.parent / (".TextureQuest-update-" + nonce))
    if not _same_path(path, stage / "request.json") or _identity(stage) != value["stage_identity"] or _identity(app) != value["app_identity"]:
        raise AppUpdateError("Application or update directory identity changed")
    installer = _installer(value["installer"], value["current_version"])
    if installer["version"] != value["target_version"] or not _same_path(_valid_process(value["parent"])["exe"], app / "TextureQuest.exe"):
        raise AppUpdateError("App-update parent or target does not match")
    return value, path, app, data, stage


def stage_downloaded_update(installer_result, archive, *, app_dir, data_dir, current_version, report=None, processes=None):
    """Stage a checksum-verified official ZIP; no network, no installed-file writes.

    ROOT's isolated onedir smoke may patch ``process_identity`` during staging to
    bind the actual separately launched fixture parent. The real helper validates
    that parent's Windows handle; no alternative parent authority exists in CLI.
    """
    processes = processes or WindowsProcesses()
    app, data = _layout(app_dir, data_dir)
    installer = _installer(installer_result, current_version)
    parent = _valid_process(processes.identity())
    if not _same_path(parent["exe"], app / "TextureQuest.exe") or not processes.alive(parent):
        raise AppUpdateError("Start the update from the installed TextureQuest app")
    processes.other_instances(app, (parent,))
    old = _bundle(app, current_version)
    if _file(archive, MAX_ZIP) != {"bytes": installer["bytes"], "sha256": installer["sha256"]}:
        raise AppUpdateError("Downloaded installer checksum differs from official metadata")
    nonce = uuid.uuid4().hex
    stage = _plain(app.parent / (".TextureQuest-update-" + nonce))
    stage.mkdir()
    _progress(report, "verify", "Verifying and staging the new app")
    payload_parent = stage / "payload"
    payload_parent.mkdir()
    payload = payload_parent / "TextureQuest"
    new = _extract(archive, payload, installer["version"], {"bytes": installer["bytes"], "sha256": installer["sha256"]})
    install = stage / "install"
    install.mkdir()
    for name in MANAGED:
        source, target = _plain(payload / name), _plain(install / name)
        if name in DIRECTORIES:
            shutil.copytree(source, target, copy_function=shutil.copy2)
        else:
            shutil.copy2(source, target)
    if _bundle(install, installer["version"]) != new or _bundle(app, current_version) != old:
        raise AppUpdateError("Application files changed while staging")
    value = {"schema": REQUEST_SCHEMA, "nonce": nonce, "app_dir": str(app), "data_dir": str(data),
             "app_identity": _identity(app), "stage_identity": _identity(stage), "current_version": current_version,
             "target_version": installer["version"], "installer": installer, "parent": parent, "old_bundle": old, "new_bundle": new}
    path = stage / "request.json"
    _write(path, value)
    _progress(report, "staged", "App update verified; preparing a safe restart")
    return {"request_path": str(path), "nonce": nonce, "target_version": installer["version"], "app_dir": str(app), "data_dir": str(data)}


def stage_update(installer_result, *, app_dir, data_dir, current_version, report=None, opener=None):
    """Refresh official release identity, download with bounds, then stage it."""
    opener = opener or urlopen
    app, data = _layout(app_dir, data_dir)
    selected = _installer(installer_result, current_version)
    metadata = versions._metadata(opener, None)
    explicit = versions._version_asset(metadata, opener, None)
    fresh_value = versions._installer(metadata, explicit)
    # A refresh may select one genuinely newer official release, never silently
    # accept changed bytes or a different envelope for the same target version.
    fresh = _installer(fresh_value, '0.0.0')
    if selected != fresh:
        fields = [key for key in selected if selected[key] != fresh[key]]
        newer = latest.version_number(fresh['version']) > latest.version_number(selected['version'])
        history = _plain(data / 'app-update-checks')
        history.mkdir(parents=True, exist_ok=True)
        if sum(1 for _ in history.glob('*.json')) >= 1024:
            raise AppUpdateError('Too many app-update check records. Existing files were preserved.')
        record = _plain(history / (uuid.uuid4().hex + '.json'))
        _write(record, dict(schema='texturequest-app-update-identity-change-v1',
            selected=selected, fresh=fresh, differing_fields=fields,
            decision='newer-official-target' if newer else 'refused',
            reason='newer-version' if newer else 'target-downgrade' if
                latest.version_number(fresh['version']) < latest.version_number(selected['version']) else 'same-version-identity-change'))
        if not newer:
            raise AppUpdateError('The published app update identity changed (' + ', '.join(fields) +
                '; offered ' + selected['version'] + ', latest ' + fresh['version'] + '). '
                'The update was refused; saved files and the installed hook are kept. Details: ' + str(record))
        _progress(report, 'refresh', 'A newer verified app is available: ' + fresh['version'] +
                  ' (previously offered ' + selected['version'] + '). Updating to ' + fresh['version'] + '.')
    fresh = _installer(fresh, current_version)
    download = _plain(app.parent / (".TextureQuest-download-" + uuid.uuid4().hex + ".zip"))
    downloaded = None
    try:
        downloaded = _download(fresh, download, opener, report)
        return stage_downloaded_update(fresh, download, app_dir=app, data_dir=data_dir,
                                       current_version=current_version, report=report)
    finally:
        if downloaded is not None and _file(download, MAX_ZIP) == downloaded:
            _plain(download).unlink()


def _receipt(stage, name, request_pin, nonce, **fields):
    return _write(stage / name, dict(schema="texturequest-app-update-" + name[:-5] + "-v1",
                                    request=request_pin, nonce=nonce, **fields))


def _bound_receipt(stage, name, pin, nonce):
    value = _load(stage / name)
    if value.get("request") != pin or value.get("nonce") != nonce:
        raise AppUpdateError("App-update response does not match this request")
    return value


def launch_updater(request, smoke_test=False, *, processes=None):
    """Return helper_ready only after exact helper/request/parent validation."""
    processes = processes or WindowsProcesses()
    value, path, app, data, stage = _request(request["request_path"], request["nonce"])
    if not _same_process(processes.identity(), value["parent"]):
        raise AppUpdateError("Only the requesting app may launch its updater")
    if _bundle(app, value["current_version"]) != value["old_bundle"] or _bundle(stage / "payload/TextureQuest", value["target_version"]) != value["new_bundle"]:
        raise AppUpdateError("App-update files changed before helper launch")
    for name in ("apply-claim.json", "helper-ready.json", "result.json"):
        if (stage / name).exists():
            raise AppUpdateError("This update request has already been launched")
    pin = _file(path, MAX_JSON)
    args = ["--apply-app-update", str(path), "--update-nonce", value["nonce"]]
    if smoke_test:
        args.append("--smoke-test")
    helper = processes.spawn(stage / "payload/TextureQuest/TextureQuest.exe", args, stage)
    deadline = time.monotonic() + READY_SECONDS
    while time.monotonic() < deadline:
        if (stage / "helper-ready.json").exists():
            ready = _bound_receipt(stage, "helper-ready.json", pin, value["nonce"])
            if not _same_process(_valid_process(ready.get("helper")), helper) or not processes.alive(helper):
                raise AppUpdateError("Updater readiness process identity changed")
            if _file(path, MAX_JSON) != pin:
                raise AppUpdateError("Update request changed during helper launch")
            return dict(request, status="helper_ready", helper=helper)
        if (stage / "result.json").exists():
            result = _bound_receipt(stage, "result.json", pin, value["nonce"])
            raise AppUpdateError(result.get("message", "Updater refused the request; current app stays open"))
        if not processes.alive(helper):
            raise AppUpdateError("Updater exited before it was ready; current app stays open")
        time.sleep(0.05)
    raise AppUpdateError("Updater did not become ready; current app stays open")


def _guard(value, path, stage):
    current, _, app, _, current_stage = _request(path, value["nonce"])
    if current != value or current_stage != stage:
        raise AppUpdateError("Update request changed during replacement")
    return app


def _move(source, destination):
    source, destination = _plain(source), _plain(destination)
    if destination.exists():
        raise AppUpdateError("Update destination already exists")
    source.rename(destination)


def apply_update(request_path, nonce, smoke_test=False, *, processes=None):
    """Staged helper CLI: wait, replace managed roots, restart, ACK or rollback.

    The processes interface is injectable in D-only transaction tests. Production
    CLI accepts no process/path overrides. Return 0 after durable commit, 2 on a
    refused request or a rolled-back update. Never kill the old app to make progress.
    """
    processes = processes or WindowsProcesses()
    value = stage = path = pin = helper = child = None
    lock = lock_id = None
    old_moved, new_moved = [], []
    parent_closed = False
    try:
        value, path, app, data, stage = _request(request_path, nonce)
        pin = _file(path, MAX_JSON)
        helper = _valid_process(processes.identity())
        if not _same_path(helper["exe"], stage / "payload/TextureQuest/TextureQuest.exe"):
            raise AppUpdateError("App updater must run from its verified staging folder")
        if _bundle(stage / "payload/TextureQuest", value["target_version"]) != value["new_bundle"]:
            raise AppUpdateError("Staged updater files changed")
        if _bundle(stage / "install", value["target_version"]) != value["new_bundle"] or _bundle(app, value["current_version"]) != value["old_bundle"]:
            raise AppUpdateError("Installed or staged replacement files changed")
        lock = app.parent / (".TextureQuest-update-lock-" + hashlib.sha256(os.path.normcase(str(app)).encode()).hexdigest()[:24] + ".json")
        _write(lock, {"nonce": nonce, "request": pin, "helper": helper})
        lock_id = _identity(lock)
        _receipt(stage, "apply-claim.json", pin, nonce, helper=helper)
        with processes.parent(value["parent"]) as wait:
            processes.other_instances(app, (value["parent"],))
            _guard(value, path, stage)
            _receipt(stage, "helper-ready.json", pin, nonce, helper=helper)
            if not wait(PARENT_SECONDS):
                raise AppUpdateError("The original app did not close; application files were not replaced")
        parent_closed = True
        processes.other_instances(app)
        _guard(value, path, stage)
        if _bundle(app, value["current_version"]) != value["old_bundle"] or _bundle(stage / "install", value["target_version"]) != value["new_bundle"]:
            raise AppUpdateError("Application files changed before replacement")
        backup, failed = _plain(stage / "backup"), _plain(stage / "failed-new")
        backup.mkdir()
        failed.mkdir()
        for name in MANAGED:
            _guard(value, path, stage)
            _move(app / name, backup / name)
            old_moved.append(name)
            _receipt(stage, "old-" + name.replace(".", "_") + ".json", pin, nonce, managed_root=name)
        for name in MANAGED:
            _guard(value, path, stage)
            _move(stage / "install" / name, app / name)
            new_moved.append(name)
            _receipt(stage, "new-" + name.replace(".", "_") + ".json", pin, nonce, managed_root=name)
        if _bundle(app, value["target_version"]) != value["new_bundle"]:
            raise AppUpdateError("Installed update verification failed")
        _guard(value, path, stage)
        processes.other_instances(app)
        args = ["--app-update-ack", str(path), "--update-nonce", nonce]
        if smoke_test:
            args.append("--smoke-test")
        child = processes.spawn(app / "TextureQuest.exe", args, app)
        _receipt(stage, "launched.json", pin, nonce, helper=helper, child=child)
        deadline = time.monotonic() + ACK_SECONDS
        while time.monotonic() < deadline:
            if (stage / "startup-ack.json").exists():
                ack = _bound_receipt(stage, "startup-ack.json", pin, nonce)
                if not _same_process(_valid_process(ack.get("child")), child) or ack.get("version") != value["target_version"]:
                    raise AppUpdateError("Restarted app acknowledgement identity mismatch")
                if _bundle(app, value["target_version"]) != value["new_bundle"]:
                    raise AppUpdateError("New application changed before commit")
                _guard(value, path, stage)
                _receipt(stage, "result.json", pin, nonce, status="committed", helper=helper, child=child,
                         version=value["target_version"], message="Application updated and restarted; portable data was untouched")
                return 0
            if not processes.alive(child):
                raise AppUpdateError("The new app exited before confirming a successful startup")
            time.sleep(0.05)
        raise AppUpdateError("The new app did not confirm a successful startup")
    except Exception as error:
        message, rollback = str(error) or type(error).__name__, "not_needed"
        try:
            if value is not None and (old_moved or new_moved):
                if child is not None and processes.alive(child):
                    processes.stop_owned(child, helper)
                _guard(value, path, stage)
                processes.other_instances(app)
                for name in reversed(new_moved):
                    _guard(value, path, stage)
                    _move(app / name, stage / "failed-new" / name)
                for name in reversed(old_moved):
                    _guard(value, path, stage)
                    _move(stage / "backup" / name, app / name)
                if _bundle(app, value["current_version"]) != value["old_bundle"]:
                    raise AppUpdateError("Restored application verification failed")
                rollback = "restored"
            if stage is not None and pin is not None and not (stage / "result.json").exists():
                _receipt(stage, "result.json", pin, nonce, status="failed", message=message, rollback=rollback)
            if parent_closed and value is not None and rollback in ("restored", "not_needed"):
                args = ["--app-update-failed", str(path), "--update-nonce", nonce]
                if smoke_test:
                    args.append("--smoke-test")
                processes.spawn(app / "TextureQuest.exe", args, app)
        except Exception as rollback_error:
            if stage is not None and pin is not None and not (stage / "rollback-error.json").exists():
                _receipt(stage, "rollback-error.json", pin, nonce, status="manual_recovery_required",
                         message=message, rollback_error=str(rollback_error))
            if parent_closed and hasattr(processes, "alert"):
                processes.alert("The app update could not finish or restart the previous app. Your data was not changed. "
                                "Application backups and details remain in " + str(stage) + ". " + str(rollback_error))
        return 2
    finally:
        # Staged files/backups remain for diagnostics. A cleanup error cannot
        # turn a durable successful commit into a failed or rolled-back update.
        if lock is not None and lock_id is not None:
            try:
                if _identity(lock) == lock_id and _load(lock).get("nonce") == nonce:
                    _plain(lock).unlink()
            except Exception as cleanup_error:
                try:
                    if stage is not None and pin is not None and not (stage / "cleanup-warning.json").exists():
                        _receipt(stage, "cleanup-warning.json", pin, nonce, message=str(cleanup_error))
                except Exception:
                    pass


def acknowledge_startup(request_path, nonce, *, app_dir, data_dir, version, processes=None):
    """Called by the restarted GUI only after its window has initialized."""
    processes = processes or WindowsProcesses()
    value, path, app, data, stage = _request(request_path, nonce)
    if (not _same_path(app_dir, app) or not _same_path(data_dir, data) or version != value["target_version"]
            or _bundle(app, version) != value["new_bundle"]):
        raise AppUpdateError("Restarted application does not match the staged update")
    pin = _file(path, MAX_JSON)
    # Popen can return immediately before the helper durably records identity.
    deadline = time.monotonic() + READY_SECONDS
    while not (stage / "launched.json").exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    launched = _bound_receipt(stage, "launched.json", pin, nonce)
    child = _valid_process(processes.identity())
    if not _same_process(child, _valid_process(launched.get("child"))) or not _same_path(child["exe"], app / "TextureQuest.exe"):
        raise AppUpdateError("Startup acknowledgement belongs to a different process")
    if not processes.alive(_valid_process(launched.get("helper"))):
        raise AppUpdateError("App updater exited before startup acknowledgement")
    return _receipt(stage, "startup-ack.json", pin, nonce, child=child, version=version)


def failure_message(request_path, nonce):
    value, path, app, data, stage = _request(request_path, nonce)
    result = _bound_receipt(stage, "result.json", _file(path, MAX_JSON), nonce)
    if result.get("status") != "failed":
        raise AppUpdateError("This app update has no failure result")
    return "The app update could not finish. " + result.get("message", "Unknown update failure") + ". The previous app was restored; your data was not changed."
