#pragma once
#include <cstdint>

namespace tq {
// Explicit private adapter ABI; does not inspect or rewrite upstream settings.
template<class Runtime> struct WorldGraphicsExports {
    using AbiFn = uint32_t (*)();
    using SetFn = void (*)(uint32_t);
    using DrawFn = void (*)(Runtime*);
    AbiFn ABI = nullptr;
    SetFn Set = nullptr;
    DrawFn Panel = nullptr;
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
};
}
