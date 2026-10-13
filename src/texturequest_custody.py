"""Windows retained file custody for nonreplacing cache commits.

FileRenameInfo uses ReplaceIfExists=FALSE. All byte/index checks and file
mutations refer to the same held native file, with write/delete sharing denied.
Directory ancestors remain held without delete sharing through each mutation.
"""
import ctypes
from ctypes import wintypes
import msvcrt
import os
from pathlib import Path

from texture_swapper import core
import texturequest_release as release

KERNEL = ctypes.WinDLL("kernel32", use_last_error=True)
KERNEL.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                              wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
KERNEL.CreateFileW.restype = wintypes.HANDLE
KERNEL.CloseHandle.argtypes = [wintypes.HANDLE]
KERNEL.CloseHandle.restype = wintypes.BOOL
KERNEL.SetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
KERNEL.SetFileInformationByHandle.restype = wintypes.BOOL


class NativeInfo(ctypes.Structure):
    _fields_ = [("attributes", wintypes.DWORD), ("created", wintypes.FILETIME),
                ("accessed", wintypes.FILETIME), ("written", wintypes.FILETIME),
                ("volume", wintypes.DWORD), ("size_high", wintypes.DWORD), ("size_low", wintypes.DWORD),
                ("links", wintypes.DWORD), ("id_high", wintypes.DWORD), ("id_low", wintypes.DWORD)]


KERNEL.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(NativeInfo)]
KERNEL.GetFileInformationByHandle.restype = wintypes.BOOL


def _information(handle):
    value = NativeInfo()
    if not KERNEL.GetFileInformationByHandle(handle, ctypes.byref(value)):
        raise ctypes.WinError(ctypes.get_last_error())
    return value


def _open(path, access, sharing, directory=False):
    handle = KERNEL.CreateFileW(str(core._plain(path)), access, sharing, None, 3,
                               0x00200000 | (0x02000000 if directory else 0x80), None)
    if not handle or handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    return handle


class DirectoryCustody:
    def __init__(self, *paths):
        self.paths = sorted({core._plain(parent) for path in paths for parent in (Path(path), *Path(path).parents)},
                            key=lambda path: (len(path.parts), str(path).casefold()))
        self.handles = {}

    def __enter__(self):
        try:
            for path in self.paths:
                handle = _open(path, 0x80 | 0x1, 3, directory=True)
                try:
                    info = _information(handle)
                    if not info.attributes & 0x10 or info.attributes & 0x400:
                        raise ValueError("Cache directory is linked or changed")
                except BaseException:
                    KERNEL.CloseHandle(handle)
                    raise
                self.handles[path] = handle
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, *_):
        for handle in reversed(list(self.handles.values())):
            KERNEL.CloseHandle(handle)
        self.handles.clear()

    def handle(self, path):
        return self.handles[core._plain(path)]


class FileCustody:
    def __init__(self, path):
        self.path = core._plain(path)
        self.handle = None
        self.stream = None
        self.initial_id = None

    def __enter__(self):
        handle = _open(self.path, 0x80000000 | 0x10000, 1)
        try:
            info = _information(handle)
            if info.attributes & (0x10 | 0x400) or info.links != 1:
                raise ValueError("Cache file is linked or not a regular file")
            fd = msvcrt.open_osfhandle(int(handle), os.O_RDONLY | os.O_BINARY)
        except BaseException:
            KERNEL.CloseHandle(handle)
            raise
        self.handle = handle
        try:
            self.stream = os.fdopen(fd, "rb")
        except BaseException:
            os.close(fd)
            self.handle = None
            raise
        self.initial_id = self.identity()
        return self

    def __exit__(self, *_):
        if self.stream is not None:
            self.stream.close()
        self.stream = self.handle = None

    def identity(self):
        info = _information(self.handle)
        if info.attributes & (0x10 | 0x400) or info.links != 1:
            raise ValueError("Held cache file changed its link identity")
        return {"volume": int(info.volume), "file_id": (int(info.id_high) << 32) | int(info.id_low)}

    def matches(self, row, pause=None):
        self.identity()
        if os.fstat(self.stream.fileno()).st_size != row["bytes"]:
            return False
        self.stream.seek(0)
        return release.hash_stream(self.stream, pause=pause) == row["sha256"]

    def checked(self, row, pause=None):
        if not self.matches(row, pause):
            raise ValueError("Archive checksum differs: " + row["name"])
        self.stream.seek(0)
        rows = core._index(self.stream, row["bytes"])
        total = 0
        for item in rows:
            release.check_pause(pause)
            total += core._payload(self.stream, item)
            if total > core.MAX_TOTAL_BYTES:
                raise ValueError("EQ2 expanded archive exceeds its limit")
        if len(rows) != row["entries"]:
            raise ValueError("Archive index differs: " + row["name"])
        self.identity()

    def rename_new(self, target, directories):
        target = core._plain(target)
        core._member(target.name)
        self.identity()
        class Rename(ctypes.Structure):
            _fields_ = [("Flags", wintypes.DWORD), ("RootDirectory", wintypes.HANDLE),
                        ("FileNameLength", wintypes.DWORD), ("FileName", wintypes.WCHAR * 1)]
        directories.handle(target.parent)  # The absolute parent path stays held.
        name = str(target).encode("utf-16-le")
        size = max(ctypes.sizeof(Rename), Rename.FileName.offset + len(name) + 2)
        buffer = ctypes.create_string_buffer(size)
        value = Rename.from_buffer(buffer)
        value.Flags = 0  # ReplaceIfExists=FALSE, never overwrite a surprise target.
        value.RootDirectory = None
        value.FileNameLength = len(name)
        ctypes.memmove(ctypes.addressof(buffer) + Rename.FileName.offset, name, len(name))
        if not KERNEL.SetFileInformationByHandle(self.handle, 3, buffer, size):
            raise ctypes.WinError(ctypes.get_last_error())
        self.path = target
        if self.identity() != self.initial_id:
            raise ValueError("Renamed cache identity differs")

    def remove(self):
        self.identity()
        class Disposition(ctypes.Structure):
            _fields_ = [("DeleteFile", ctypes.c_ubyte)]
        value = Disposition(1)
        if not KERNEL.SetFileInformationByHandle(self.handle, 4, ctypes.byref(value), ctypes.sizeof(value)):
            raise ctypes.WinError(ctypes.get_last_error())
