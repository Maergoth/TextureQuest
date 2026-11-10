#pragma once
#include <cstdint>

namespace tq {
// Explicit private adapter ABI; does not inspect or rewrite upstream settings.
template<class Runtime> struct WorldGraphicsExports {
    using AbiFn = uint32_t (*)();
    using SetFn = void (*)(uint32_t);
    using DrawFn = void (*)(Runtime*);
    using PresetFn = int32_t (*)();
    using ApplyPresetFn = uint32_t (*)(uint32_t);
    AbiFn ABI = nullptr;
    SetFn Set = nullptr;
    DrawFn Panel = nullptr;
    DrawFn Advanced = nullptr;
    DrawFn Debug = nullptr;
    PresetFn Preset = nullptr;
    ApplyPresetFn ApplyPreset = nullptr;
    bool Compatible() const { return ABI && Set && Panel && ABI() == 1; }
    bool Reconcile(bool selected, bool original) const {
        if (!selected || !Compatible()) return false;
        Set(original ? 0u : 1u);
        return true;
    }
    bool Draw(Runtime* runtime) const {
        if (!runtime || !Compatible()) return false;
        Panel(runtime);
        return true;
    }
    bool PresetsCompatible() const { return Compatible() && Advanced && Debug && Preset && ApplyPreset; }
    bool ReadPreset(int32_t& preset) const {
        if (!PresetsCompatible()) return false;
        const int32_t value = Preset();
        preset = value >= 0 && value < 4 ? value : -1;
        return true;
    }
    bool RequestPreset(uint32_t preset) const {
        return preset < 4 && PresetsCompatible() && ApplyPreset(preset) != 0;
    }
    bool DrawAdvanced(Runtime* runtime) const {
        if (!runtime || !Compatible()) return false;
        (Advanced ? Advanced : Panel)(runtime);
        return true;
    }
    bool DrawDebug(Runtime* runtime) const {
        if (!runtime || !Compatible() || !Debug) return false;
        Debug(runtime);
        return true;
    }
};
}
