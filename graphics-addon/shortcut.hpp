#pragma once
// Foreground-only shortcut, same TQKEY1 file and behaviour as 1.7.0: the key is
// registered only while EQ2 owns the foreground window and re-read every second.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>

namespace tq {

inline bool KeyAllowed(unsigned vk, unsigned modifiers) {
    return modifiers <= 7 && ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5a) || (vk >= 0x60 && vk <= 0x6b) ||
                              (vk >= 0x6d && vk <= 0x6f) || (vk >= 0x70 && vk <= 0x87) || vk == 0x20 ||
                              (vk >= 0x21 && vk <= 0x28) || vk == 0x2d || vk == 0x2e);
}

inline void ReadShortcut(const std::wstring& path, unsigned& vk, unsigned& modifiers) {
    vk = 0x6a;
    modifiers = 0;
    FILE* file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!file)
        return;
    char text[64] = {};
    const size_t read = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    unsigned a, b;
    if (read && sscanf_s(text, "TQKEY1\n%u\n%u", &a, &b) == 2 && KeyAllowed(a, b)) {
        vk = a;
        modifiers = b;
    }
}

class Shortcut {
public:
    Shortcut(std::wstring path, std::atomic<bool>& original) : Path(std::move(path)), Original(original) {}

    void Start() { Thread = CreateThread(nullptr, 0, &Shortcut::Run, this, 0, nullptr); }

    void Stop() {
        Stopping = true;
        if (Thread) {
            WaitForSingleObject(Thread, INFINITE);
            CloseHandle(Thread);
            Thread = nullptr;
        }
    }

private:
    static DWORD WINAPI Run(void* self) {
        static_cast<Shortcut*>(self)->Loop();
        return 0;
    }

    void Loop() {
        HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        if (!window)
            return;
        const DWORD process = GetCurrentProcessId();
        unsigned vk = 0, modifiers = 0, active_vk = 0, active_modifiers = 0;
        bool registered = false;
        ULONGLONG next_read = 0;
        while (!Stopping) {
            if (GetTickCount64() >= next_read) {
                ReadShortcut(Path, vk, modifiers);
                next_read = GetTickCount64() + 1000;
            }
            DWORD foreground = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
            const bool want = foreground == process;
            if (registered && (!want || vk != active_vk || modifiers != active_modifiers)) {
                UnregisterHotKey(window, 0x5451);
                registered = false;
            }
            if (want && !registered && RegisterHotKey(window, 0x5451, modifiers | MOD_NOREPEAT, vk)) {
                registered = true;
                active_vk = vk;
                active_modifiers = modifiers;
            }
            MSG message;
            while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
                if (message.message == WM_HOTKEY)
                    Original = !Original;
            }
            Sleep(50);
        }
        if (registered)
            UnregisterHotKey(window, 0x5451);
        DestroyWindow(window);
    }

    std::wstring Path;
    std::atomic<bool>& Original;
    std::atomic<bool> Stopping{false};
    HANDLE Thread = nullptr;
};

}
