"""Owned per-user background-loader installation; never modifies EQ2 assets.

The native watcher owns interception. This adapter prepares its full-pack input,
records an explicit installation and manages only its own HKCU Run value/event.
Absent runtime packaging is an unavailable capability, not a successful install.
"""
from __future__ import annotations

from contextlib import contextmanager, redirect_stdout
import ctypes
from ctypes import wintypes
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import sys
import time
import types
import uuid

from texture_swapper import core
from texturequest_direct import DirectSwapper

SCHEMA = "texturequest-sideload-runtime-v1"
PREPARER_SHA256 = "1eb75e7ef468a555d14f2b3e5355cecfa9bc25559c44c2b6e05ff393c4e73a17"
PORTABLE_PREPARER_SHA256 = "2959a254c551ec1cea3bcd7ab59e8210cdaecab1224d825ed18b65840c1087d9"
PORTABLE_ROLES = {"watcher": "TextureQuestWatch.exe", "dll": "TextureQuestBackground.dll",
                  "helper": "preparer/prepare_full_pack.py", "catalog": "catalog/HANDOFF.json"}
MAX_JSON = 4 * 1024 * 1024
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
STATES = {"armed", "attaching", "awaiting_index", "attached", "restart_needed", "refresh_required", "error", "stopped"}


class LoaderError(RuntimeError):
    pass


class WatcherStartError(LoaderError):
    def __init__(self, message, started_pid=None):
        super().__init__(message)
        self.started_pid = started_pid


def _need(condition, message):
    if not condition:
        raise LoaderError(message)


def _canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _fingerprint(path):
    value = core._plain(path).stat()
    return (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)


def _bytes(path, limit=MAX_JSON):
    path = core._plain(path)
    before = _fingerprint(path)
    _need(before[2] <= limit and path.is_file(), "Loader metadata exceeds its supported limit.")
    data = path.read_bytes()
    _need(len(data) == before[2] and _fingerprint(path) == before, "Loader metadata changed while reading.")
    return data


def _json(path):
    value = core._json(_bytes(path))
    _need(type(value) is dict, "Loader metadata must be an object.")
    return value


class _LiveInfo(ctypes.Structure):
    _fields_ = [("attributes", wintypes.DWORD), ("created", wintypes.FILETIME),
        ("accessed", wintypes.FILETIME), ("written", wintypes.FILETIME), ("volume", wintypes.DWORD),
        ("size_high", wintypes.DWORD), ("size_low", wintypes.DWORD), ("links", wintypes.DWORD),
        ("id_high", wintypes.DWORD), ("id_low", wintypes.DWORD)]


def _live_api():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                  wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(_LiveInfo)]
    kernel.GetFileInformationByHandle.restype = wintypes.BOOL
    kernel.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    kernel.ReadFile.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    return kernel


def _open_live(path):
    # Native status publication replaces its pathname. Readers must allow that
    # rename and may finish reading the previous, complete owned FILE object.
    handle = _live_api().CreateFileW(str(path), 0x80000000, 0x1 | 0x4, None, 3, 0x00200000 | 0x80, None)
    if not handle or handle == wintypes.HANDLE(-1).value:
        error = ctypes.get_last_error()
        if error in (2, 3):
            return None
        raise OSError(error, "Cannot open the live background status")
    return handle


def _live_window(handle):
    info = _LiveInfo()
    if not _live_api().GetFileInformationByHandle(handle, ctypes.byref(info)):
        raise OSError(ctypes.get_last_error(), "Cannot query the live status FILE")
    _need(not info.attributes & (0x10 | 0x400) and info.links <= 1, "Live status is not an ordinary FILE.")
    # Link count may become zero when a permitted atomic replacement unlinks
    # this old version. No pathname or link-count equality is required.
    return (info.volume, (info.id_high << 32) | info.id_low, (info.size_high << 32) | info.size_low,
            (info.written.dwHighDateTime << 32) | info.written.dwLowDateTime)


def _read_live(handle, count):
    buffer = ctypes.create_string_buffer(count)
    read = wintypes.DWORD()
    if not _live_api().ReadFile(handle, buffer, count, ctypes.byref(read), None):
        raise OSError(ctypes.get_last_error(), "Cannot read the live background status")
    return buffer.raw[:read.value]


def _close_live(handle):
    _need(_live_api().CloseHandle(handle), "Cannot confirm closing the live status FILE.")


def _live_status_bytes(path):
    handle = _open_live(core._plain(path))
    if handle is None:
        return None
    try:
        before = _live_window(handle)
        _need(before[2] <= MAX_JSON, "Live background status exceeds its supported limit.")
        chunks, total = [], 0
        while True:
            block = _read_live(handle, min(65536, MAX_JSON + 1 - total))
            if not block:
                break
            total += len(block)
            _need(total <= MAX_JSON, "Live background status exceeds its supported limit.")
            chunks.append(block)
        _need(_live_window(handle) == before and total == before[2], "Live status changed inside its retained FILE window.")
        return b"".join(chunks)
    finally:
        _close_live(handle)


def _pin(path, expected=None):
    path = core._plain(path)
    before = _fingerprint(path)
    _need(path.is_file() and before[2] <= 256 * 1024 * 1024, "Invalid loader runtime FILE.")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    _need(_fingerprint(path) == before, "Loader runtime FILE changed while reading.")
    result = dict(path=str(path), bytes=before[2], sha256=digest.hexdigest(), stat5=list(before))
    if expected is not None:
        _need(type(expected) is dict and type(expected.get("bytes")) is int and
              expected["bytes"] == result["bytes"] and expected.get("sha256") == result["sha256"],
              "Loader runtime FILE does not match its bundled hash: " + path.name)
    return result


def _sealed(value):
    body = {key: item for key, item in value.items() if key != "record_sha256"}
    return dict(body, record_sha256=_sha(_canonical(body)))


def _read_sealed(path):
    value = _json(path)
    _need(value == _sealed(value), "Loader installation receipt changed; preserve it for recovery.")
    return value


def _exclusive_json(path, value):
    path = core._plain(path)
    with path.open("xb") as stream:
        stream.write(_canonical(value))
        stream.flush()
        os.fsync(stream.fileno())


def _portable_spec(spec):
    """Admit a complete, location-independent payload identity."""
    _need(type(spec) is dict and set(spec) == {"schemaVersion", "development", "payload_id", "files", "roles"} and
          type(spec["schemaVersion"]) is int and spec["schemaVersion"] == 2 and spec["development"] is False and
          spec["roles"] == PORTABLE_ROLES, "Invalid portable loader runtime descriptor.")
    rows = spec["files"]
    _need(type(rows) is list and 1 <= len(rows) <= 64, "Invalid portable loader FILE roster.")
    names = []
    for row in rows:
        _need(type(row) is dict and set(row) == {"path", "size", "sha256"} and type(row["path"]) is str,
              "Invalid portable loader FILE entry.")
        core._member(row["path"])
        _need(row["path"].isascii() and type(row["size"]) is int and 0 <= row["size"] <= 256 * 1024 * 1024 and
              type(row["sha256"]) is str and re.fullmatch("[0-9a-f]{64}", row["sha256"]) is not None,
              "Invalid portable loader FILE pin.")
        names.append(row["path"])
    required = {*PORTABLE_ROLES.values(), "LICENSE-Detours.md", "catalog/current-archives.json",
                *("catalog/members-%02d.json" % number for number in range(7))}
    _need(names == sorted(names) and len({name.casefold() for name in names}) == len(names) and set(names) == required,
          "Portable loader FILE roster has unknown, missing or aliased members.")
    canonical = json.dumps(rows, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("utf-8")
    _need(spec["payload_id"] == _sha(canonical), "Portable loader payload identity differs from its complete FILE roster.")
    helper = next(row for row in rows if row["path"] == PORTABLE_ROLES["helper"])
    _need(helper["sha256"] == PORTABLE_PREPARER_SHA256, "This portable preparer revision has not been admitted.")
    return spec


def _portable_files(root, spec, *, component=False):
    """Check every FILE and directory, including names absent from the roster."""
    root = core._plain(root)
    _need(root.is_dir(), "Portable loader payload directory is missing.")
    expected = {row["path"] for row in spec["files"]}
    directories = {parent.as_posix() for name in expected for parent in Path(name).parents if str(parent) != "."}
    actual, seen_directories = set(), set()
    def walk_error(error):
        raise error
    for folder, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
        here = core._plain(folder, root)
        for name in list(dirs):
            child = core._plain(here / name, root)
            relative = child.relative_to(root).as_posix()
            if component and relative == "background-epochs":
                dirs.remove(name)
                continue
            seen_directories.add(relative)
            _need(relative in directories, "Unknown portable loader directory is preserved: " + relative)
        for name in files:
            path = core._plain(here / name, root)
            relative = path.relative_to(root).as_posix()
            if component and relative == "COMPONENT.json":
                continue
            _need(path.is_file(), "Portable loader member is not an ordinary FILE.")
            actual.add(relative)
    _need(actual == expected and seen_directories == directories, "Portable loader FILE membership differs from its complete roster.")
    return {row["path"]: _pin(core._plain(root / row["path"], root),
                             dict(bytes=row["size"], sha256=row["sha256"])) for row in spec["files"]}


def admit_portable_payload(directory):
    """Read-only builder admission; descriptor and sideload payload are siblings."""
    directory = core._plain(directory)
    descriptor = directory / "texturequest-sideload-runtime.json"
    pin = _pin(descriptor)
    spec = _portable_spec(_json(descriptor))
    pins = _portable_files(directory / "sideload", spec)
    _pin(descriptor, pin)
    return dict(spec=spec, descriptor=pin, payload_root=str(directory / "sideload"), pins=list(pins.values()))


def _disjoint(a, b):
    a, b = core._plain(a), core._plain(b)
    return not (a.is_relative_to(b) or b.is_relative_to(a))


def _read_run(name):
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_QUERY_VALUE) as key:
            command, kind = winreg.QueryValueEx(key, name)
    except FileNotFoundError:
        return None
    _need(kind == winreg.REG_SZ and type(command) is str, "The loader startup value has an unknown type.")
    return command


def _write_run(name, expected, command):
    import winreg
    _need(_read_run(name) == expected, "A different startup entry is preserved; it was not replaced.")
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE | winreg.KEY_QUERY_VALUE) as key:
        winreg.SetValueEx(key, name, 0, winreg.REG_SZ, command)
    _need(_read_run(name) == command, "The loader startup registration was not confirmed.")


def _remove_run(name, command):
    import winreg
    actual = _read_run(name)
    _need(actual in (None, command), "A different startup entry is preserved; it was not removed.")
    if actual is not None:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
            winreg.DeleteValue(key, name)
    _need(_read_run(name) is None, "The loader startup entry is still present.")


def _process_identity(pid):
    """Query only; PID reuse never counts as the retained watcher/game session."""
    _need(type(pid) is int and 0 < pid <= 0xffffffff, "Invalid loader process PID.")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME)]
    kernel.GetProcessTimes.restype = wintypes.BOOL
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetExitCodeProcess.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        if ctypes.get_last_error() == 87:
            return None
        raise OSError(ctypes.get_last_error(), "Cannot query the loader/game process")
    try:
        exit_code = wintypes.DWORD()
        _need(kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code)), "Cannot query the loader/game exit state.")
        if exit_code.value != 259:
            return None
        creation, exit_time, system, user = (wintypes.FILETIME() for _ in range(4))
        _need(kernel.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_time), ctypes.byref(system), ctypes.byref(user)),
              "Cannot query the loader/game birth time.")
        buffer = ctypes.create_unicode_buffer(32768)
        size = wintypes.DWORD(len(buffer))
        _need(kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)), "Cannot query the loader/game executable.")
        return dict(pid=pid, birth100ns=(creation.dwHighDateTime << 32) | creation.dwLowDateTime, exe=buffer.value)
    finally:
        _need(kernel.CloseHandle(handle), "Cannot confirm closing the loader process-query handle.")


def _same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def _alive(identity, executable):
    current = _process_identity(identity["pid"])
    return bool(current and current["birth100ns"] == identity["birth100ns"] and _same_path(current["exe"], executable))


def _spawn_watcher(arguments):
    try:
        process = subprocess.Popen(arguments, cwd=str(Path(arguments[0]).parent), stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True, creationflags=subprocess.CREATE_NO_WINDOW)
    except Exception as error:
        raise WatcherStartError("The background watcher could not start: " + str(error)) from error
    try:
        identity = _process_identity(process.pid)
        _need(identity is not None and _same_path(identity["exe"], arguments[0]), "The background watcher did not remain running.")
        return identity
    except Exception as error:
        raise WatcherStartError("The watcher launch outcome is unknown: " + str(error), process.pid) from error


def _signal_stop(event_name):
    _need(re.fullmatch(r"Local\\TQWatch-[0-9a-f]{32}", event_name) is not None, "Invalid owned loader stop event.")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.OpenEventW.restype = wintypes.HANDLE
    kernel.SetEvent.argtypes = [wintypes.HANDLE]
    kernel.SetEvent.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenEventW(0x0002, False, event_name)
    if not handle:
        raise OSError(ctypes.get_last_error(), "Cannot open this loader's stop event")
    try:
        _need(kernel.SetEvent(handle), "Cannot signal this loader's stop event.")
    finally:
        _need(kernel.CloseHandle(handle), "Cannot confirm closing the loader stop-event handle.")


class _Progress(io.TextIOBase):
    def __init__(self, callback):
        self.callback, self.pending = callback, ""

    def write(self, text):
        self.pending += text
        while "\n" in self.pending:
            line, self.pending = self.pending.split("\n", 1)
            if line and self.callback:
                self.callback(line)
        return len(text)


@contextmanager
def _preparer(runtime):
    """Import only the exact reviewed FILE, on an explicit installation worker."""
    source = runtime["files"]["preparer"]
    raw = _bytes(source["path"], 256 * 1024)
    _need(len(raw) == source["bytes"] and _sha(raw) == source["sha256"], "The reviewed preparer FILE changed before import.")
    admitted = PORTABLE_PREPARER_SHA256 if runtime.get("portable") else PREPARER_SHA256
    _need(source["sha256"] == admitted, "This preparer revision has not been admitted.")
    module_name = "_texturequest_fullpack_" + uuid.uuid4().hex
    module = types.ModuleType(module_name)
    module.__file__ = source["path"]
    sys.modules[module_name] = module
    try:
        exec(compile(raw, source["path"], "exec"), module.__dict__)
        yield module
    finally:
        sys.modules.pop(module_name, None)


def _ready_bindings_unchanged(ready):
    """Choose reuse before full verification; never recover from its failure."""
    _need(ready.get("schema") == "tq-fullpack-ready-v1" and type(ready.get("providers")) is list and
          type(ready.get("catalog_inputs")) is list and type(ready.get("bound_files")) is list,
          "Saved preparation has invalid complete FILE membership; it is preserved.")
    expected = [ready["source_vpl"], ready["private_vpl"], ready["prepared"], *ready["catalog_inputs"]]
    expected.extend(pin for row in ready["providers"] for pin in (row["source"], row["released"], row["output"]))
    known = {}
    for pin in expected:
        _need(type(pin) is dict and type(pin.get("path")) is str and type(pin.get("bytes")) is int and
              type(pin.get("sha256")) is str and re.fullmatch("[0-9a-f]{64}", pin["sha256"]) is not None and
              type(pin.get("stat5")) is list and len(pin["stat5"]) == 5 and
              all(type(item) is int for item in pin["stat5"]) and pin["stat5"][2] == pin["bytes"],
              "Saved preparation FILE membership is malformed; it is preserved.")
        key = str(core._plain(pin["path"])).casefold()
        prior = known.setdefault(key, pin)
        _need(all(prior[field] == pin[field] for field in ("bytes", "sha256", "stat5")), "Saved preparation repeats a conflicting FILE binding.")
    bound = {str(core._plain(pin["path"])).casefold(): pin for pin in ready["bound_files"]}
    _need(len(bound) == len(ready["bound_files"]) and set(bound) == set(known), "Saved preparation lacks its complete bound FILE set.")
    for key, pin in known.items():
        _need(all(bound[key].get(field) == pin[field] for field in ("bytes", "sha256", "stat5")), "Saved preparation bound FILE identity differs.")
        try:
            if _fingerprint(pin["path"]) != tuple(pin["stat5"]):
                return False
        except (FileNotFoundError, NotADirectoryError):
            return False
    return True


def _require_restored_sources(ready):
    """Use fully verified archive bytes, never stale installer bookkeeping."""
    _need(type(ready.get("providers")) is list, "Verified preparation lacks its provider source proofs.")
    installed = []
    for row in ready["providers"]:
        source, released = row["source"], row["released"]
        _need(all(type(pin.get("sha256")) is str and re.fullmatch("[0-9a-f]{64}", pin["sha256"]) is not None
                  for pin in (source, released)), "Verified provider lacks its source/released archive SHA.")
        if source["sha256"] == released["sha256"]:
            installed.append(row["name"])
    _need(not installed, "The game still contains released TextureQuest PAKs: " + ", ".join(installed) +
          ". Run the official EQ2 patcher to restore game files, then install the background loader again.")


def _prepare(runtime, game, prepared, output, progress):
    with _preparer(runtime) as helper, redirect_stdout(_Progress(progress)):
        reuse = False
        if runtime.get("prepared_ready"):
            ready_pin = runtime["prepared_ready"]
            _pin(ready_pin["path"], ready_pin)
            ready_path = Path(ready_pin["path"])
            ready = _json(ready_path)
            _need(_same_path(ready.get("game", ""), game) and
                  _same_path(ready.get("prepared", {}).get("path", ""), prepared), "Saved full-pack preparation belongs to another game or download cache.")
            reuse = _ready_bindings_unchanged(ready)
        if reuse:
            verified = helper.verify_ready(ready_path)
        else:
            if runtime.get("prepared_ready") and progress:
                progress("Saved preparation FILEs changed; verifying current sources into a new owned preparation.")
            catalog = runtime["files"]["catalog"]
            ready = helper.prepare(game, prepared, output, catalog_path=catalog["path"], catalog_sha=catalog["sha256"])
            ready_path = output / "READY.json"
            verified = helper.verify_ready(ready_path)
        _need(type(verified) is dict and verified.get("status") == "VERIFIED_READY" and
              _same_path(verified.get("game", ""), game) and verified.get("manifest") == ready.get("manifest"),
              "Full-pack verification did not confirm the exact game and routing manifest.")
        _need(ready.get("schema") == "tq-fullpack-ready-v1" and _same_path(ready["game"], game), "The full-pack preparation did not confirm this game.")
        _require_restored_sources(ready)
        manifest = _pin(ready["manifest"]["path"], ready["manifest"])
        bindings = _make_bindings(ready, output.parent / "BINDINGS.bin")
        return dict(ready_json=_pin(ready_path), manifest=manifest, bindings=bindings, output=ready["output"],
                    source_vpl=ready["source_vpl"], prepared=ready["prepared"], applied=ready["applied"])


def _make_bindings(ready, path):
    """Project the complete verified route union; native leases recheck freshness."""
    _need(ready.get("schema") == "tq-fullpack-ready-v1" and type(ready.get("routes")) is list and
          1 <= len(ready["routes"]) <= 1024 and ready.get("route_count") == len(ready["routes"]), "Invalid full-pack routes for native freshness bindings.")
    pins = {}
    for pin in ready["bound_files"]:
        key = os.path.normcase(str(core._plain(pin["path"])))
        _need(key not in pins, "Duplicate full-pack FILE binding.")
        pins[key] = pin
    selected = {}
    for route in ready["routes"]:
        for role in ("original", "replacement"):
            file_path = core._plain(route[role])
            key = os.path.normcase(str(file_path))
            _need(key in pins, "A full-pack route lacks its verified source/replacement FILE binding.")
            selected[key] = (file_path, pins[key])
    _need(1 <= len(selected) <= 2048, "Native freshness binding count is outside the supported limit.")
    chunks = [b"TQCFWB01", struct.pack("<II", 1, len(selected))]
    for key in sorted(selected):
        file_path, pin = selected[key]
        info = pin["stat5"]
        _need(type(info) is list and len(info) == 5 and all(type(item) is int for item in info) and
              type(pin["bytes"]) is int and 0 <= pin["bytes"] <= 2**63 - 1 and info[2] == pin["bytes"] and
              0 <= info[3] < 2**63 and info[3] % 100 == 0 and _fingerprint(file_path) == tuple(info), "A verified full-pack FILE changed before native bindings publication.")
        value = str(file_path)
        encoded = value.encode("utf-16-le")
        units = len(encoded) // 2
        _need(file_path.is_absolute() and re.fullmatch("[A-Za-z]:", file_path.drive) is not None and
              "\0" not in value and 1 <= units <= 32767, "Invalid native freshness binding path.")
        chunks.extend([struct.pack("<IIQq", units, 0, pin["bytes"], info[3]), encoded])
    blob = b"".join(chunks)
    _need(len(blob) <= 4 * 1024 * 1024, "Native freshness bindings exceed 4 MiB.")
    with core._plain(path).open("xb") as stream:
        stream.write(blob)
        stream.flush()
        os.fsync(stream.fileno())
    return _pin(path)


def _owner_bytes(game, installation_id, manifest_sha, bindings_sha):
    value = str(core._plain(game)).replace("/", "\\")
    value = "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in value)
    encoded = value.encode("utf-16-le")
    _need(1 <= len(encoded) // 2 <= 32767 and "\0" not in value and
          re.fullmatch("[0-9a-f]{32}", installation_id) is not None and
          all(re.fullmatch("[0-9a-f]{64}", digest) is not None for digest in (manifest_sha, bindings_sha)), "Invalid installed status ownership fields.")
    blob = b"TQBGOWN2" + struct.pack("<II", 2, len(encoded) // 2) + installation_id.encode("ascii") + manifest_sha.encode("ascii") + bindings_sha.encode("ascii") + encoded
    _need(len(blob) <= 1024 * 1024, "Installed status ownership exceeds its limit.")
    return blob


class LoaderManager:
    def __init__(self, state_dir, *, development_manifest=None):
        self.state_dir = core._plain(state_dir)
        self.base = core._plain(self.state_dir / "sideload", self.state_dir)
        self.registrations = self.base / "registrations"
        self._control_available()
        self.development = development_manifest is not None
        _need(not (development_manifest and getattr(sys, "frozen", False)), "Development loader descriptors are unavailable in distributed builds.")
        self.descriptor = core._plain(development_manifest) if self.development else Path(__file__).resolve().parent / "texturequest-sideload-runtime.json"
        self.runtime_cache = None
        self.runtime_error = "The background loader runtime is not included in this app build."
        self.history_cache = {}
        self.dead_sessions = set()

    def _control_available(self):
        transfer = self.base / "CONTROL-TRANSFER.json"
        _need(not (transfer.exists() or transfer.is_symlink()),
              "Background loader control moved from this preview. Use the Desktop TextureQuest app; this old preview cannot manage or fork its installation.")

    def installed_game(self):
        """Discover one owned installation for the ordinary double-click flow."""
        self._control_available()
        if not self.registrations.exists():
            return None
        games = []
        for path in core._plain(self.registrations, self.base).iterdir():
            _need(path.is_file() and re.fullmatch("[0-9a-f]{24}\\.json", path.name) is not None,
                  "Unknown loader registration entry.")
            saved = _read_sealed(path)
            game = self._game(saved["game"])
            _need(path == self._path(game), "Loader registration filename differs from its game identity.")
            if self._status(game)["installed"]:
                games.append(game)
        return games[0] if len(games) == 1 else None

    def _component_runtime(self, admitted, component):
        spec = admitted["spec"]
        component = core._plain(component, self.base / "runtimes")
        marker = _read_sealed(component / "COMPONENT.json")
        _need(marker == _sealed(dict(schema="tq-portable-loader-component-v1", state_dir=str(self.state_dir),
                                   component_root=str(component), descriptor=spec)),
              "Portable loader component ownership changed; it is preserved.")
        members = _portable_files(component, spec, component=True)
        files = {("preparer" if role == "helper" else role): members[name] for role, name in spec["roles"].items()}
        return dict(portable=dict(payload_id=spec["payload_id"], component_root=str(component),
                                  state_dir=str(self.state_dir), spec=spec), files=files,
                    preparation_root=str(component / "background-epochs"),
                    descriptor=admitted["descriptor"], pins=list(members.values()))

    def _materialize(self, admitted, game):
        """Only explicit Install creates an immutable owned payload component."""
        spec = _portable_spec(admitted["spec"])
        root = core._plain(self.base / "runtimes", self.base)
        component = core._plain(root / spec["payload_id"], root)
        _need(_disjoint(self.state_dir, game) and _disjoint(component, game),
              "TextureQuest data and loader components must be outside the selected game folder.")
        if component.exists():
            return self._component_runtime(admitted, component)
        # Re-admit source membership before copying. No unknown prior stage or
        # component is adopted, removed, repaired or overwritten.
        _portable_files(admitted["payload_root"], spec)
        root.mkdir(parents=True, exist_ok=True)
        stage = core._plain(root / (".stage-" + uuid.uuid4().hex), root)
        stage.mkdir()
        for row in spec["files"]:
            source = core._plain(Path(admitted["payload_root"]) / row["path"], admitted["payload_root"])
            destination = core._plain(stage / row["path"], stage)
            destination.parent.mkdir(parents=True, exist_ok=True)
            with source.open("rb") as incoming, destination.open("xb") as outgoing:
                shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
                outgoing.flush()
                os.fsync(outgoing.fileno())
        _portable_files(stage, spec)
        _portable_files(admitted["payload_root"], spec)
        _pin(admitted["descriptor"]["path"], admitted["descriptor"])
        _exclusive_json(stage / "COMPONENT.json", _sealed(dict(schema="tq-portable-loader-component-v1",
            state_dir=str(self.state_dir), component_root=str(component), descriptor=spec)))
        _need(not component.exists(), "A concurrent portable component is preserved; installation stopped.")
        os.rename(stage, component)
        return self._component_runtime(admitted, component)

    def _runtime(self):
        if not self.descriptor.exists():
            self.runtime_error = "The background loader runtime is not included in this app build."
            return None
        try:
            if not self.development:
                admitted = admit_portable_payload(self.descriptor.parent)
                self.runtime_error = ""
                return dict(bundle=admitted, portable_pending=True)
            if self.runtime_cache is not None and all(_fingerprint(pin["path"]) == tuple(pin["stat5"]) for pin in self.runtime_cache["pins"]):
                return self.runtime_cache
            descriptor_pin = _pin(self.descriptor)
            spec = _json(self.descriptor)
            _need(spec.get("schema") == SCHEMA and type(spec.get("development")) is bool, "Invalid loader runtime descriptor.")
            _need(spec["development"] == self.development, "A development loader runtime requires an explicit development descriptor.")
            _need(type(spec.get("files")) is dict and set(spec["files"]) == {"watcher", "dll", "preparer", "catalog"}, "Incomplete loader runtime roles.")
            files = {}
            for role, record in spec["files"].items():
                _need(type(record) is dict and type(record.get("path")) is str, "Invalid loader runtime role path.")
                relative = Path(record["path"])
                _need(self.development or not relative.is_absolute(), "Bundled runtime roles must be relative paths.")
                path = core._plain(relative if relative.is_absolute() else self.descriptor.parent / relative)
                _need(self.development or path.is_relative_to(self.descriptor.parent), "Loader runtime role escaped the bundle.")
                files[role] = _pin(path, record)
            _need(files["preparer"]["sha256"] == PREPARER_SHA256, "This preparer revision has not been admitted.")
            _need(Path(files["watcher"]["path"]).parent == Path(files["dll"]["path"]).parent,
                  "The native watcher and its pinned DLL must share their runtime directory.")
            _need(Path(files["watcher"]["path"]).name.casefold() == "texturequestwatch.exe" and
                  Path(files["dll"]["path"]).name.casefold() == "texturequestbackground.dll",
                  "The native watcher/DLL filenames do not match their fixed loader protocol.")
            # V2's reviewed PACKAGE_ROOT is the native bundle root, one level
            # above its preparer-v2 source directory. Never infer it from user input.
            package = Path(files["watcher"]["path"]).parent
            _need(package.drive.lower() == "d:", "This runtime still requires D: preparation support and is not ready for portable installation.")
            runtime = dict(files=files, preparation_root=str(package / "background-epochs"), descriptor=descriptor_pin,
                           pins=[descriptor_pin, *files.values()])
            for role in ("prepared_ready", "prepared_source"):
                if role in spec:
                    value = spec[role]
                    _need(self.development and type(value) is dict and type(value.get("path")) is str,
                          "Pre-prepared paths require an explicit development descriptor.")
                    runtime[role] = _pin(value["path"], value)
                    runtime["pins"].append(runtime[role])
            self.runtime_cache, self.runtime_error = runtime, ""
            return runtime
        except (OSError, ValueError, TypeError, KeyError, LoaderError) as error:
            self.runtime_cache = None
            self.runtime_error = "Background loader unavailable: " + str(error)
            return None

    def _game(self, game):
        _need(isinstance(game, (str, Path)) and str(game).strip() and Path(game).is_absolute(), "Choose the installed EverQuest II game folder.")
        return core._plain(game)

    def _key(self, game):
        return _sha(os.path.normcase(str(game)).encode("utf-8"))[:24]

    def _path(self, game):
        return self.registrations / (self._key(game) + ".json")

    def _record(self, game):
        path = core._plain(self._path(game), self.base)
        if not path.exists():
            return None
        record = _read_sealed(path)
        _need(record.get("schema") in {"tq-background-installation-v1", "tq-background-installation-v2"} and _same_path(record.get("game", ""), game)
              and record.get("game_key") == self._key(game), "This loader receipt belongs to another game. Choose its installed game folder.")
        _need(re.fullmatch("[0-9a-f]{32}", record.get("installation_id", "")) is not None and
              type(record.get("revision")) is int and record["revision"] >= 1 and
              record.get("phase") in {"preparing", "prepared", "registered", "installed", "failed", "uninstalling", "uninstalled"}, "Invalid loader installation identity/state.")
        _need(type(record.get("runtime")) is dict and set(record["runtime"].get("files", {})) == {"watcher", "dll", "preparer", "catalog"}, "Loader runtime receipt changed.")
        runtime = record["runtime"]
        portable = runtime.get("portable")
        _need((record["schema"] == "tq-background-installation-v2") == (portable is not None),
              "Portable and legacy installation receipt contracts cannot be mixed.")
        if portable is not None:
            _need(type(portable) is dict and set(portable) == {"payload_id", "component_root", "state_dir", "spec"},
                  "Portable component receipt changed.")
            spec = _portable_spec(portable["spec"])
            component = core._plain(self.base / "runtimes" / spec["payload_id"], self.base)
            _need(portable["payload_id"] == spec["payload_id"] and portable["component_root"] == str(component) and
                  portable["state_dir"] == str(self.state_dir) and _disjoint(self.state_dir, game) and _disjoint(component, game) and
                  runtime["preparation_root"] == str(component / "background-epochs"), "Portable loader paths changed.")
            verified = self._component_runtime(dict(spec=spec, descriptor=runtime["descriptor"]), component)
            _need(all(runtime["files"][role] == verified["files"][role] for role in verified["files"]),
                  "Portable loader role identities changed.")
        owned = Path(runtime["preparation_root"]) / record["installation_id"]
        _need((portable is not None or owned.drive.lower() == "d:") and record.get("epoch_root") == str(owned) and
              record.get("status_dir") == str(owned / "watcher") and record.get("stop_event") == "Local\\TQWatch-" + record["installation_id"] and
              record.get("run_name") == "TextureQuestLoader-" + record["game_key"], "Loader ownership fields changed.")
        if portable is not None:
            _need(record.get("prepared", {}).get("path") == str(self.state_dir / "direct-prepared.json"),
                  "Portable installation must use its normal downloaded-pack metadata.")
            if record.get("ready"):
                ready = record["ready"]
                _need(ready.get("output") == str(owned / "pack") and
                      ready.get("ready_json", {}).get("path") == str(owned / "pack" / "READY.json") and
                      ready.get("manifest", {}).get("path") == str(owned / "pack" / "READY.bin") and
                      ready.get("bindings", {}).get("path") == str(owned / "BINDINGS.bin"),
                      "Portable preparation paths escaped their owned component epoch.")
        if record.get("arguments"):
            _need(record["arguments"] == self._arguments(record) and record.get("run_command") == subprocess.list2cmdline(record["arguments"]), "Loader startup command changed.")
        if record.get("owner"):
            _need(record["owner"].get("path") == str(owned / "watcher" / "OWNER.bin"), "Loader status ownership path changed.")
        return record

    def _publish(self, record, previous):
        """Owned immutable history first, then CAS of the one active registration."""
        path = self._path(Path(record["game"]))
        actual = _bytes(path) if path.exists() else None
        _need((actual is None and previous is None) or
              (actual is not None and previous is not None and core._json(actual) == previous), "Loader registration changed concurrently; existing files are preserved.")
        value = _sealed(record)
        home = core._plain(self.base / "installations" / value["installation_id"], self.base)
        _exclusive_json(home / (f"receipt-{value['revision']:04d}.json"), value)
        core._atomic(path, value)
        return value

    def _advance(self, record, phase, **values):
        return self._publish(dict(record, revision=record["revision"] + 1, phase=phase, **values), record)

    def _arguments(self, record):
        return [record["runtime"]["files"]["watcher"]["path"], "--game-dir", record["game"],
                "--manifest", record["ready"]["manifest"]["path"], "--manifest-sha256", record["ready"]["manifest"]["sha256"],
                "--bindings", record["ready"]["bindings"]["path"], "--bindings-sha256", record["ready"]["bindings"]["sha256"],
                "--status-dir", record["status_dir"], "--stop-event", record["stop_event"]]

    def _document_sessions(self, record, value):
        _need(value.get("schema") == "tq-background-status-v2" and value.get("state") in STATES and
              value.get("install_nonce") == record["installation_id"] and
              value.get("owner_sha256") == record.get("owner", {}).get("sha256") and
              type(value.get("run_id")) is str and re.fullmatch("[0-9a-f]{32}", value["run_id"]) is not None and
              type(value.get("watcher_pid")) is int and value["watcher_pid"] > 0 and
              type(value.get("watcher_birth100ns")) is int and value["watcher_birth100ns"] > 0 and
              type(value.get("message")) is str and type(value.get("sessions")) is list and len(value["sessions"]) <= 4096,
              "Invalid or unowned background watcher status.")
        rows, aliases = [], set()
        for row in value["sessions"]:
            _need(type(row) is dict and type(row.get("pid")) is int and row["pid"] > 0 and
                  type(row.get("birth100ns")) is int and row["birth100ns"] >= 0 and type(row.get("state")) is str and
                  type(row.get("redirects")) is int and row["redirects"] >= 0 and
                  type(row.get("private_index_active")) is bool and type(row.get("observation_only")) is bool,
                  "Invalid background game-session status.")
            key = (row["pid"], row["birth100ns"])
            _need(key not in aliases, "Duplicate background game-session identity.")
            aliases.add(key)
            if row["birth100ns"] == 0:
                _need(row["observation_only"] and row["redirects"] == 0 and not row["private_index_active"] and
                      row["state"] == "restart_needed", "Unknown identity is not a never-attached diagnostic row.")
            else:
                rows.append(row)
        return rows

    def _history_sessions(self, record):
        history = core._plain(Path(record["status_dir"]) / "history", record["epoch_root"])
        if not history.exists():
            return []
        rows, total, runs = [], 0, 0
        for folder in history.iterdir():
            _need(folder.is_dir() and re.fullmatch("run-[0-9a-f]{32}", folder.name) is not None,
                  "Unknown background run history is preserved.")
            folder = core._plain(folder, history)
            runs += 1
            _need(runs <= 4096, "Background run history exceeds its supported bound; it is preserved.")
            previous = core._plain(folder / "previous-status.json", history)
            if not previous.exists():
                continue
            stamp = _fingerprint(previous)
            total += stamp[2]
            _need(total <= 32 * 1024 * 1024, "Background run history exceeds its supported byte bound; it is preserved.")
            key = (str(previous), record["installation_id"], record["owner"]["sha256"])
            cached = self.history_cache.get(key)
            if cached is None or cached[0] != stamp:
                cached = (stamp, self._document_sessions(record, _json(previous)))
                self.history_cache[key] = cached
            rows.extend(cached[1])
        return rows

    def _live_sessions(self, record, rows):
        selected, aliases = [], set()
        for row in rows:
            key = (row["pid"], row["birth100ns"])
            if key in aliases or key in self.dead_sessions:
                continue
            aliases.add(key)
            if _alive(row, Path(record["game"]) / "EverQuest2.exe"):
                selected.append(row)
            else:
                self.dead_sessions.add(key)
        return selected

    def _observation(self, record):
        value_path = core._plain(Path(record["status_dir"]) / "status.json", record["epoch_root"])
        data = _live_status_bytes(value_path)
        if data is None:
            identity = record.get("watcher")
            _need(identity or not record.get("spawn_requested"),
                  "The watcher startup outcome is unknown. Its startup entry and cached files are preserved until owned status is available.")
            running = bool(identity and _alive(identity, record["runtime"]["files"]["watcher"]["path"]))
            _need(not identity or record["phase"] in {"registered", "uninstalled"},
                  "The retained watcher status is missing. Session ownership is unknown; cached files are preserved.")
            sessions = self._live_sessions(record, record.get("retained_sessions", []))
            return dict(running=running, active_sessions=len(sessions), sessions=sessions, state="starting" if running else "absent", message="")
        value = core._json(data)
        _need(type(value) is dict and record.get("owner"), "Background status lacks its installed ownership marker.")
        expected_owner = _owner_bytes(record["game"], record["installation_id"], record["ready"]["manifest"]["sha256"], record["ready"]["bindings"]["sha256"])
        _need(_bytes(record["owner"]["path"], 1024 * 1024) == expected_owner and _sha(expected_owner) == record["owner"]["sha256"],
              "Installed status ownership marker changed.")
        current_rows = self._document_sessions(record, value)
        running = _alive(dict(pid=value["watcher_pid"], birth100ns=value["watcher_birth100ns"]), record["runtime"]["files"]["watcher"]["path"])
        retained = self._live_sessions(record, [*current_rows, *record.get("retained_sessions", []), *self._history_sessions(record)])
        return dict(running=running, active_sessions=len(retained), sessions=retained, state=value["state"], message=value["message"])

    def _status(self, game, runtime=None):
        record = self._record(game)
        if record is None:
            return dict(installed=False, running=False, active_sessions=0, available=runtime is not None,
                        message="Automatically detects future EverQuest II sessions. Game files stay unchanged." if runtime else self.runtime_error)
        observed = self._observation(record)
        actual_run = _read_run(record["run_name"])
        expected = record.get("run_command")
        _need(actual_run is None or expected is not None and actual_run == expected, "A different startup entry is preserved. Cannot confirm this loader's ownership.")
        installed = bool(actual_run is not None or record["phase"] in {"registered", "installed", "uninstalling"} or
                         observed["running"] and record["phase"] != "uninstalled")
        message = observed["message"]
        if record["phase"] == "uninstalled":
            _need(actual_run is None, "An uninstalled loader still has a startup entry; preserve it for recovery.")
            message = "Uninstalled for future launches. Current game sessions keep their textures until they exit." if observed["active_sessions"] else "Background loader uninstalled. Game files were not changed."
        elif installed and not runtime:
            message = "The loader runtime is unavailable. Uninstall remains available; active game sessions are left alone."
        elif installed and not observed["running"]:
            message = "Background loader installed; its watcher is not running. Uninstall and reinstall to start it again."
        elif record["phase"] == "failed" and not installed:
            message = "The previous loader preparation failed and was preserved. Install creates a fresh attempt. " + record.get("error", "")
        elif observed["state"] == "restart_needed":
            message = message or "Loader installed. This game session started too early; restart the game when convenient."
        elif observed["state"] == "refresh_required":
            message = message or "The game assets changed after patching. Refresh the prepared texture pack before another hooked session."
        elif installed and not message:
            message = "Loader installed and armed for future EverQuest II sessions; an already open game stays unchanged."
        return dict(installed=installed, running=observed["running"], active_sessions=observed["active_sessions"],
                    available=runtime is not None or installed, message=message)

    def status(self, game):
        self._control_available()
        runtime = self._runtime()
        if not str(game).strip():
            return dict(installed=False, running=False, active_sessions=0, available=runtime is not None,
                        message="Choose your EverQuest II game folder." if runtime else self.runtime_error)
        return self._status(self._game(game), runtime)

    def cache_in_use(self):
        """Thread-safe filesystem/process snapshot, with no UI/Tk dependency."""
        try:
            self._control_available()
        except Exception:
            return True
        if not self.registrations.exists():
            return False
        try:
            for path in core._plain(self.registrations, self.base).iterdir():
                _need(path.is_file() and re.fullmatch("[0-9a-f]{24}\\.json", path.name) is not None, "Unknown loader registration entry.")
                record = _read_sealed(path)
                status = self._status(self._game(record["game"]))
                if status["installed"] or status["running"] or status["active_sessions"] or record["phase"] in {"preparing", "prepared"}:
                    return True
            return False
        except Exception:
            return True

    def install(self, game, prepared, progress=None):
        runtime = self._runtime()
        _need(runtime is not None, self.runtime_error)
        game = self._game(game)
        _need(_disjoint(self.state_dir, game), "TextureQuest data must be outside the selected game folder.")
        self.base.mkdir(parents=True, exist_ok=True)
        self.registrations.mkdir(exist_ok=True)
        with core._lock(self.state_dir / "operation.lock"), core._lock(self.base / "helper-owner.lock"):
            self._control_available()
            current = self._record(game)
            status = self._status(game, runtime)
            _need(not status["installed"] and not status["running"] and not status["active_sessions"], "Uninstall the existing loader and let its game sessions exit before installing.")
            _need(current is None or current["phase"] in {"failed", "uninstalled"}, "An unfinished loader installation is preserved; do not replay it.")
            _need(_read_run("TextureQuestLoader-" + self._key(game)) is None,
                  "A different startup entry is preserved; it was not replaced.")
            core.validate_game(game)
            source = runtime.get("prepared_source")
            prepared_path = core._plain(source["path"]) if source else core._plain(self.state_dir / "direct-prepared.json", self.state_dir)
            if source:
                _need(self.development, "External downloaded-pack metadata requires a development descriptor.")
                _pin(prepared_path, source)
            saved = _json(prepared_path)
            _need(saved.get("schema") == "texturequest-direct-prepared-v1" and saved == prepared, "The selected downloaded pack no longer matches direct-prepared.json.")
            if runtime.get("portable_pending"):
                runtime = self._materialize(runtime["bundle"], game)
            installation_id = uuid.uuid4().hex
            home = core._plain(self.base / "installations" / installation_id, self.base)
            _need(not home.exists(), "Unknown loader installation directory is preserved.")
            home.mkdir(parents=True)
            epoch = core._plain(Path(runtime["preparation_root"]) / installation_id)
            _need(not epoch.exists(), "Unknown loader runtime epoch is preserved.")
            epoch.mkdir(parents=True)
            (epoch / "watcher").mkdir()
            record = dict(schema="tq-background-installation-v2" if runtime.get("portable") else "tq-background-installation-v1", installation_id=installation_id, revision=1, phase="preparing",
                game=str(game), game_key=self._key(game), epoch_root=str(epoch), status_dir=str(epoch / "watcher"), stop_event="Local\\TQWatch-" + installation_id,
                run_name="TextureQuestLoader-" + self._key(game), runtime=runtime, prepared=_pin(prepared_path), created=time.time())
            _exclusive_json(home / "INTENT.json", _sealed(record))
            _exclusive_json(epoch / "OWNER.json", _sealed(dict(installation_id=installation_id, game=str(game), state_dir=str(self.state_dir))))
            record = self._publish(record, current)
            spawned_identity = None
            try:
                output = core._plain(epoch / "pack")
                _need(not output.exists(), "Unknown full-pack output is preserved.")
                if progress:
                    progress("Verifying the full texture pack against current game files. Game files remain read-only.")
                ready = _prepare(runtime, game, prepared_path, output, progress)
                record = self._advance(record, "prepared", ready=ready)
                owner_path = Path(record["status_dir"]) / "OWNER.bin"
                owner_bytes = _owner_bytes(game, installation_id, ready["manifest"]["sha256"], ready["bindings"]["sha256"])
                with owner_path.open("xb") as stream:
                    stream.write(owner_bytes)
                    stream.flush()
                    os.fsync(stream.fileno())
                record = self._advance(record, "prepared", owner=_pin(owner_path))
                arguments = self._arguments(record)
                record = self._advance(record, "prepared", arguments=arguments, run_command=subprocess.list2cmdline(arguments))
                _write_run(record["run_name"], None, record["run_command"])
                record = self._advance(record, "registered")
                record = self._advance(record, "registered", spawn_requested=True)
                spawned_identity = _spawn_watcher(arguments)
                record = self._advance(record, "registered", watcher=spawned_identity)
                deadline = time.monotonic() + 10
                while True:
                    observed = self._observation(record)
                    if observed["running"] and observed["state"] in {"armed", "attaching", "awaiting_index", "attached", "restart_needed"}:
                        break
                    _need(observed["state"] not in {"error", "stopped"}, observed["message"] or "The background watcher did not arm.")
                    _need(time.monotonic() < deadline, "Startup registration was saved, but the background watcher did not confirm it armed. Uninstall remains available.")
                    time.sleep(.05)
                record = self._advance(record, "installed")
                return self._status(game, runtime)
            except Exception as error:
                # No unknown process kill, registry rollback, artifact removal or automatic retry.
                try:
                    evidence = {}
                    if spawned_identity:
                        evidence["watcher"] = spawned_identity
                    elif isinstance(error, WatcherStartError):
                        evidence.update(spawn_requested=error.started_pid is not None, possible_watcher_pid=error.started_pid)
                    self._advance(record, "failed", error=str(error), **evidence)
                except Exception:
                    pass
                raise

    def uninstall(self, game, progress=None):
        game = self._game(game)
        with core._lock(self.state_dir / "operation.lock"), core._lock(self.base / "helper-owner.lock"):
            self._control_available()
            record = self._record(game)
            _need(record is not None, "No owned loader installation exists for this game. Choose its installed game folder.")
            observed = self._observation(record)
            command = record.get("run_command")
            actual_run = _read_run(record["run_name"])
            _need(actual_run is None or command is not None and actual_run == command, "Unknown loader startup ownership is preserved.")
            if record["phase"] == "uninstalled":
                _need(_read_run(record["run_name"]) is None, "Unexpected startup entry is preserved.")
                return self._status(game, self._runtime())
            record = self._advance(record, "uninstalling", retained_sessions=observed["sessions"])
            if command is not None:
                _remove_run(record["run_name"], command)
            if observed["running"]:
                _signal_stop(record["stop_event"])
            record = self._advance(record, "uninstalled")
            if progress:
                progress("Background loader removed for future launches. Active game sessions are left untouched.")
            return self._status(game, self._runtime())
