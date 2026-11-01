#pragma once
// Calls only the explicit ABI of our separately installed managed AIO build.
// Never loads/injects a DLL, sends a key or edits another add-on's preferences.
#include <windows.h>
#include <mutex>
#include "texturequest_aio_api.h"

namespace tq {
struct AioSnapshot {
    bool Present = false, Compatible = false, ControlsCompatible = false;
    TextureQuestAIOStatusV1 Status{};
    TextureQuestAIOControlStatusV2 Controls{};
};
class AioController {
    std::mutex Lock;
    HMODULE Module = nullptr;
    TextureQuestAIOSetMasterFn Set = nullptr;
    TextureQuestAIOGetStatusFn Get = nullptr;
    TextureQuestAIOSetControlsFn SetControls = nullptr;
    TextureQuestAIOGetControlsFn GetControls = nullptr;
    AioSnapshot Last{};
    void ReadControls() {
        TextureQuestAIOControlStatusV2 controls{};
        Last.ControlsCompatible = SetControls && GetControls &&
            GetControls(TEXTUREQUEST_AIO_CONTROLS_ABI, &controls, sizeof(controls)) &&
            controls.Abi == TEXTUREQUEST_AIO_CONTROLS_ABI && controls.Bytes == sizeof(controls);
        if (Last.ControlsCompatible) Last.Controls = controls;
    }
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
            SetControls = reinterpret_cast<TextureQuestAIOSetControlsFn>(GetProcAddress(Module, "TextureQuestAIOSetControls"));
            GetControls = reinterpret_cast<TextureQuestAIOGetControlsFn>(GetProcAddress(Module, "TextureQuestAIOGetControls"));
            Last.Present = true;
        }
        TextureQuestAIOStatusV1 status{};
        Last.Compatible = Set && Get && Set(TEXTUREQUEST_AIO_ABI, original ? 0u : 1u) &&
            Get(TEXTUREQUEST_AIO_ABI, &status, sizeof(status)) &&
            status.Abi == TEXTUREQUEST_AIO_ABI && status.Bytes == sizeof(status);
        if (Last.Compatible) Last.Status = status;
        ReadControls();
    }
    AioSnapshot Snapshot() {
        std::lock_guard guard(Lock);
        return Last;
    }
    bool RequestControls(const TextureQuestAIOControlsV2& controls) {
        std::lock_guard guard(Lock);
        if (!Last.ControlsCompatible || !SetControls ||
            !SetControls(TEXTUREQUEST_AIO_CONTROLS_ABI, &controls, sizeof(controls))) return false;
        ReadControls(); // Queued request only; never waits on rendering/model work.
        return true;
    }
    void Clear() {
        std::lock_guard guard(Lock);
        if (Set) Set(TEXTUREQUEST_AIO_ABI, 0);
        Set = nullptr; Get = nullptr; SetControls = nullptr; GetControls = nullptr;
        if (Module) FreeLibrary(Module);
        Module = nullptr; Last = {};
    }
};
} // namespace tq
