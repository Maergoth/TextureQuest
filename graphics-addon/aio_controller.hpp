#pragma once
// Calls only the explicit ABI of our separately installed managed AIO build.
// Never loads/injects a DLL, sends a key or edits another add-on's preferences.
#include <windows.h>
#include <mutex>
#include "texturequest_aio_api.h"

namespace tq {
struct AioSnapshot {
    bool Present = false, Compatible = false;
    TextureQuestAIOStatusV1 Status{};
};
class AioController {
    std::mutex Lock;
    HMODULE Module = nullptr;
    TextureQuestAIOSetMasterFn Set = nullptr;
    TextureQuestAIOGetStatusFn Get = nullptr;
    AioSnapshot Last{};
public:
    void Reconcile(bool selected, bool original) {
        if (!selected) return; // Never acquire a user's unrelated AIO module.
        std::lock_guard guard(Lock);
        if (!Module) {
            // Take a normal reference so a concurrent unregister/unload cannot
            // unmap code while an atomic setter/status call is in progress.
            if (!GetModuleHandleExW(0, L"standalone-dlssnr.addon64", &Module)) return;
            Set = reinterpret_cast<TextureQuestAIOSetMasterFn>(GetProcAddress(Module, "TextureQuestAIOSetMaster"));
            Get = reinterpret_cast<TextureQuestAIOGetStatusFn>(GetProcAddress(Module, "TextureQuestAIOGetStatus"));
            Last.Present = true;
        }
        TextureQuestAIOStatusV1 status{};
        Last.Compatible = Set && Get && Set(TEXTUREQUEST_AIO_ABI, original ? 0u : 1u) &&
            Get(TEXTUREQUEST_AIO_ABI, &status, sizeof(status)) &&
            status.Abi == TEXTUREQUEST_AIO_ABI && status.Bytes == sizeof(status);
        if (Last.Compatible) Last.Status = status;
    }
    AioSnapshot Snapshot() {
        std::lock_guard guard(Lock);
        return Last;
    }
    void Clear() {
        std::lock_guard guard(Lock);
        if (Set) Set(TEXTUREQUEST_AIO_ABI, 0);
        Set = nullptr; Get = nullptr;
        if (Module) FreeLibrary(Module);
        Module = nullptr; Last = {};
    }
};
} // namespace tq
