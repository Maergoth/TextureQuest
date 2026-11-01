#pragma once
// Owns only TextureQuest's exact FX technique. The shortcut thread changes the
// existing atomic master state; runtime/render callbacks apply it here.
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace tq {

inline bool EffectsEnabled(bool selected, bool original) noexcept {
    return selected && !original;
}

// Runtime is effect_runtime in the add-on and a CPU fake in the self-check.
// No graphics resources or device operations are needed by this controller.
template <class Runtime>
class EffectsController {
    using Technique = decltype(std::declval<Runtime*>()->find_technique("", ""));
    struct Entry { Technique Handle{}; };
    std::mutex Lock;
    std::unordered_map<Runtime*, Entry> Runtimes;

    static Technique FindOwned(Runtime* runtime) {
        Technique handle = runtime->find_technique("TQEffects.fx", "TQBloom");
        if (!handle.handle)
            return {};
        // A filename and technique name form the ownership boundary. Never
        // search globally or accept another effect's same-named technique.
        char file[256]{}, name[128]{};
        size_t file_size = sizeof(file), name_size = sizeof(name);
        runtime->get_technique_effect_name(handle, file, &file_size);
        runtime->get_technique_name(handle, name, &name_size);
        file[sizeof(file) - 1] = '\0';
        name[sizeof(name) - 1] = '\0';
        if (file_size > sizeof(file) || name_size > sizeof(name) ||
            std::strcmp(file, "TQEffects.fx") || std::strcmp(name, "TQBloom"))
            return {};
        return handle;
    }

    static void Apply(Runtime* runtime, const Entry& entry, bool enabled) {
        if (entry.Handle.handle && runtime->get_technique_state(entry.Handle) != enabled)
            runtime->set_technique_state(entry.Handle, enabled);
    }

public:
    // Initialization may precede asynchronous FX compilation. The later
    // reloaded-effects event supplies the newly valid handles in that case.
    void Reload(Runtime* runtime, bool enabled) {
        if (!runtime)
            return;
        std::lock_guard lock(Lock);
        Entry& entry = Runtimes[runtime];
        entry = {}; // All previous technique handles are invalid after reload.
        entry.Handle = FindOwned(runtime);
        Apply(runtime, entry, enabled);
    }

    // Per-frame, never per-draw. Resolve once for a runtime already alive when
    // this add-on was loaded; misses are retried only at an FX reload event.
    void Reconcile(Runtime* runtime, bool enabled) {
        if (!runtime)
            return;
        std::lock_guard lock(Lock);
        auto [it, added] = Runtimes.try_emplace(runtime);
        if (added)
            it->second.Handle = FindOwned(runtime);
        Apply(runtime, it->second, enabled);
    }

    // A resetting/destroying runtime must not receive any more API calls.
    void Forget(Runtime* runtime) {
        std::lock_guard lock(Lock);
        Runtimes.erase(runtime);
    }

    // Called only after runtime callbacks are unregistered during orderly
    // add-on unload. Remaining entries refer to live effect runtimes.
    void DisableAndClear() {
        std::lock_guard lock(Lock);
        for (const auto& [runtime, entry] : Runtimes)
            Apply(runtime, entry, false);
        Runtimes.clear();
    }
};

}
