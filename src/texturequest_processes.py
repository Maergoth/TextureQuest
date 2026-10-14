"""Fast read-only Windows game/patcher census; never launches or opens a process."""
import ctypes
from ctypes import wintypes

GUARDED = frozenset({'everquest2.exe', 'eq2.exe', 'launchpad.exe', 'eq2patch.exe', 'eq2patcher.exe'})


class ProcessEntry(ctypes.Structure):
    _fields_ = [('size', wintypes.DWORD), ('usage', wintypes.DWORD),
                ('pid', wintypes.DWORD), ('heap', ctypes.c_size_t),
                ('module', wintypes.DWORD), ('threads', wintypes.DWORD),
                ('parent', wintypes.DWORD), ('priority', wintypes.LONG),
                ('flags', wintypes.DWORD), ('name', wintypes.WCHAR * 260)]


def process_names():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    for name in ('Process32FirstW', 'Process32NextW'):
        getattr(kernel, name).argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
        getattr(kernel, name).restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if not handle or handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = ProcessEntry()
        entry.size = ctypes.sizeof(entry)
        if not kernel.Process32FirstW(handle, ctypes.byref(entry)):
            raise ctypes.WinError(ctypes.get_last_error())
        names = []
        while True:
            names.append(entry.name)
            if not kernel.Process32NextW(handle, ctypes.byref(entry)):
                error = ctypes.get_last_error()
                if error != 18:
                    raise ctypes.WinError(error)
                break
        return names
    finally:
        if not kernel.CloseHandle(handle):
            raise ctypes.WinError(ctypes.get_last_error())


def ensure_closed():
    try:
        names = process_names()
        if not names:
            raise OSError('Empty process census')
    except OSError as error:
        raise RuntimeError('Unable to verify that EQ2 and its patcher are closed') from error
    active = [name for name in names if name.casefold() in GUARDED]
    if active:
        raise RuntimeError('Close EverQuest II and its patcher first: ' + ', '.join(active))
