#pragma once
#include <windows.h>
#include <mutex>
#include "world_graphics_api.hpp"

namespace tq {
class WorldGraphicsController {
    std::mutex Lock;
    HMODULE Module = nullptr;
    WorldGraphicsExports<reshade::api::effect_runtime> API;
    bool Resolve() {
        if (!Module) {
            // Reference only the addon already loaded by ReShade. No injection
            // or LoadLibrary; retain code while calling its explicit ABI.
            if (!GetModuleHandleExW(0, L"EQ2Graphics.addon64", &Module)) return false;
            API.ABI = reinterpret_cast<decltype(API.ABI)>(GetProcAddress(Module, "TextureQuestWorldGraphicsABI"));
            API.Set = reinterpret_cast<decltype(API.Set)>(GetProcAddress(Module, "TextureQuestSetWorldGraphicsEnabled"));
            API.Panel = reinterpret_cast<decltype(API.Panel)>(GetProcAddress(Module, "TextureQuestDrawWorldGraphics"));
        }
        return API.Compatible();
    }
public:
    bool Reconcile(bool selected, bool original) {
        if (!selected) return false;
        std::lock_guard guard(Lock);
        return Resolve() && API.Reconcile(true, original);
    }
    bool Draw(reshade::api::effect_runtime* runtime) {
        std::lock_guard guard(Lock);
        return Resolve() && API.Draw(runtime);
    }
    void Clear() {
        std::lock_guard guard(Lock);
        if (API.Compatible()) API.Set(0);
        API = {};
        if (Module) FreeLibrary(Module);
        Module = nullptr;
    }
};
}
