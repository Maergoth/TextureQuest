#pragma once
// Project-owned effects only. A menu request is distinct from the effective
// master/depth gate, so a disabled effect is never forced back on every frame.
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace tq {
inline constexpr char EffectsFile[] = "TQEffects.fx";
inline constexpr std::array<const char*, 3> EffectNames = {
    "TQAmbientOcclusion", "TQContactShadows", "TQDepthDiagnostic"
};
inline constexpr std::array<const char*, 3> RetiredEffectNames = {
    "TQBloom", "TQIndirectLighting", "TQAntiAliasing"
};
inline constexpr uint32_t DefaultEffectRequests = 0x03; // Debug is opt-in.
inline constexpr uint32_t DepthEffectRequests = 0x07;
inline constexpr size_t MaxEffectRuntimes = 4;
inline bool EffectsEnabled(bool selected, bool original) noexcept { return selected && !original; }

// Only small copied facts cross into the overlay. No technique, texture or
// resource handle survives its render callback.
struct EffectSnapshot {
    uint32_t Requested = DefaultEffectRequests, Available = 0, Active = 0;
    bool DepthReady = false;
};
inline const char* EffectStatus(const EffectSnapshot& state, size_t index, bool selected, bool original) {
    if (index >= EffectNames.size() || !selected || !(state.Requested & (1u << index))) return "Off";
    if (original) return "Original";
    if (!(state.Available & (1u << index))) return "Shader unavailable";
    if ((DepthEffectRequests & (1u << index)) && !state.DepthReady) return "Waiting for depth";
    return (state.Active & (1u << index)) ? "On" : "Off";
}

// Public SDK docs include NUL in returned sizes; ReShade 6.8 caller-buffer
// getters return the copied character count instead. Accept either exact count,
// but always require capacity and byte equality for the entire name PLUS NUL.
inline bool ExactEffectText(const char* text, size_t reported, const char* expected, size_t capacity) {
    if (!text || !expected) return false;
    const size_t length = std::strlen(expected);
    return length < capacity && (reported == length || reported == length + 1) &&
        std::memcmp(text, expected, length + 1) == 0;
}

// Runtime is the real SDK interface or the CPU fake. Only runtime identity,
// requested bits, copied presence/disabled facts and render-thread identity
// survive a callback: no FX, SRV or resource handle is cached or reused.
template<class Runtime>
class EffectsController {
    using Technique = decltype(std::declval<Runtime*>()->find_technique("", ""));
    struct Entry {
        Runtime* RuntimeIdentity = nullptr;
        std::thread::id RenderThread{};
        uint32_t Requested = DefaultEffectRequests;
        EffectSnapshot Last{};
        bool MasterKnown = false, LastMaster = false;
        bool CleanIdle = false, CurrentAbsent = false, RetiredClean = false;
    };
    std::mutex Lock;
    std::array<Entry, MaxEffectRuntimes> Entries{};
    inline static thread_local const EffectsController* Writing = nullptr;
    struct WriteScope {
        const EffectsController* Previous;
        explicit WriteScope(const EffectsController* value) : Previous(Writing) { Writing = value; }
        ~WriteScope() { Writing = Previous; }
    };
    Entry* Find(Runtime* runtime) {
        for (auto& entry : Entries) if (runtime && entry.RuntimeIdentity == runtime) return &entry;
        return nullptr;
    }
    Entry* Ensure(Runtime* runtime) {
        if (!runtime) return nullptr;
        if (auto* entry = Find(runtime)) return entry;
        for (auto& entry : Entries) if (!entry.RuntimeIdentity) {
            entry = {runtime, {}, DefaultEffectRequests, {}};
            return &entry;
        }
        return nullptr;
    }
    static void Invalidate(Entry& entry) {
        entry.CleanIdle = entry.CurrentAbsent = entry.RetiredClean = false;
    }
    static bool Exact(const char* text, size_t size, const char* expected, size_t capacity) {
        return ExactEffectText(text, size, expected, capacity);
    }
    static int OwnedIndex(Runtime* runtime, Technique technique) {
        if (!technique.handle) return -1;
        char file[256]{}, name[128]{};
        size_t a = sizeof(file), b = sizeof(name);
        runtime->get_technique_effect_name(technique, file, &a);
        runtime->get_technique_name(technique, name, &b);
        if (!Exact(file, a, EffectsFile, sizeof(file))) return -1;
        // Old owned caches can survive later user search-path changes.
        for (const char* retired : RetiredEffectNames)
            if (Exact(name, b, retired, sizeof(name))) return -2;
        for (size_t i = 0; i < EffectNames.size(); ++i)
            if (Exact(name, b, EffectNames[i], sizeof(name))) return static_cast<int>(i);
        return -1;
    }
public:
    // The Home-list callback receives a current technique handle. Validate its
    // exact identity synchronously, outside Lock; never retain it in UI state.
    static int OwnedTechniqueIndex(Runtime* runtime, Technique technique) {
        return runtime ? OwnedIndex(runtime, technique) : -1;
    }

    // Init/reload/destroy are metadata-only, even if ReShade raises them from a
    // compiler or shutdown thread. Reload retains all in-session menu requests.
    void Reload(Runtime* runtime, bool = false) {
        std::lock_guard guard(Lock);
        if (auto* entry = Ensure(runtime)) {
            entry->Last = {};
            Invalidate(*entry);
        }
    }
    void Forget(Runtime* runtime) {
        std::lock_guard guard(Lock);
        if (auto* entry = Find(runtime)) *entry = {};
    }
    void Clear() {
        std::lock_guard guard(Lock);
        Entries = {}; // No SDK call from AddonUninit's possibly non-render thread.
    }

    // Overlay seams are metadata only and may be called from its own thread.
    // Activation remains deferred to the next guarded render reconciliation.
    EffectSnapshot Snapshot(Runtime* runtime) {
        std::lock_guard guard(Lock);
        if (const auto* entry = Find(runtime)) {
            auto result = entry->Last;
            result.Requested = entry->Requested;
            return result;
        }
        return {};
    }
    bool SetRequested(Runtime* runtime, size_t index, bool enabled) {
        std::lock_guard guard(Lock);
        auto* entry = Find(runtime);
        if (!entry || index >= EffectNames.size()) return false;
        const uint32_t bit = 1u << index;
        if (enabled) entry->Requested |= bit;
        else entry->Requested &= ~bit;
        Invalidate(*entry);
        return true;
    }

    // Called only by genuine render callbacks. Probe must query fresh binding
    // facts synchronously; it is never retained. An extra runtime or a later
    // foreign thread receives no SDK calls, including no depth probe.
    template<class DepthProbe>
    bool Reconcile(Runtime* runtime, bool masterEnabled, DepthProbe&& probe) {
        std::lock_guard guard(Lock);
        Entry* entry = Ensure(runtime);
        if (!entry) return false;
        const auto current = std::this_thread::get_id();
        if (entry->RenderThread == std::thread::id{}) entry->RenderThread = current;
        if (entry->RenderThread != current) return false;
        if (!entry->MasterKnown || entry->LastMaster != masterEnabled) {
            Invalidate(*entry);
            entry->MasterKnown = true;
            entry->LastMaster = masterEnabled;
        }
        // Only absence/verified-disabled facts survive callbacks. Reloads,
        // requests, owned technique events and master edges invalidate them.
        if (entry->CleanIdle || entry->CurrentAbsent) return true;
        if (!masterEnabled || !entry->Requested) {
            struct Sweep { EffectSnapshot Observed{}; bool Clean = true; } sweep;
            WriteScope ownWrite(this);
            // Multiple copies of our file can survive search-path changes.
            // Confirm every current/retired owned technique is off before
            // allowing idle callbacks to skip SDK access altogether.
            runtime->enumerate_techniques(EffectsFile, [](Runtime* current, Technique technique, void* data) {
                auto& state = *static_cast<Sweep*>(data);
                const int index = OwnedIndex(current, technique);
                if (index < 0 && index != -2) return;
                if (index >= 0) state.Observed.Available |= 1u << index;
                if (current->get_technique_state(technique)) current->set_technique_state(technique, false);
                if (current->get_technique_state(technique)) {
                    state.Clean = false;
                    if (index >= 0) state.Observed.Active |= 1u << index;
                }
            }, &sweep);
            entry->Last = sweep.Observed;
            entry->CleanIdle = entry->RetiredClean = sweep.Clean;
            entry->CurrentAbsent = sweep.Clean && !sweep.Observed.Available;
            return true;
        }
        const bool depth = masterEnabled && (entry->Requested & DepthEffectRequests) && probe(runtime);
        EffectSnapshot observed{};
        observed.DepthReady = depth;
        WriteScope ownWrite(this);
        if (!entry->RetiredClean) {
            bool hasRetired = false;
            for (const char* name : RetiredEffectNames)
                if (OwnedIndex(runtime, runtime->find_technique(EffectsFile, name)) == -2) {
                    hasRetired = true;
                    break;
                }
            bool retiredClean = true;
            if (hasRetired) {
                runtime->enumerate_techniques(EffectsFile, [](Runtime* current, Technique technique, void* data) {
                    if (OwnedIndex(current, technique) != -2) return;
                    if (current->get_technique_state(technique)) current->set_technique_state(technique, false);
                    if (current->get_technique_state(technique)) *static_cast<bool*>(data) = false;
                }, &retiredClean);
            }
            entry->RetiredClean = retiredClean;
        }
        for (size_t i = 0; i < EffectNames.size(); ++i) {
            // Fresh exact-file lookup each callback: handles never outlive it.
            const auto technique = runtime->find_technique(EffectsFile, EffectNames[i]);
            if (OwnedIndex(runtime, technique) != static_cast<int>(i)) continue;
            const uint32_t bit = 1u << i;
            observed.Available |= bit;
            const bool enabled = masterEnabled && (entry->Requested & bit) && (!(DepthEffectRequests & bit) || depth);
            if (runtime->get_technique_state(technique) != enabled)
                runtime->set_technique_state(technique, enabled);
            if (runtime->get_technique_state(technique)) observed.Active |= bit;
        }
        entry->Last = observed;
        entry->CurrentAbsent = !observed.Available && entry->RetiredClean;
        return true;
    }

    // ReShade calls this BEFORE changing a technique. Own synchronous writes
    // must be accepted before the mutex/any SDK call to avoid deadlock and to
    // prevent a master/readiness disable becoming a new menu request.
    bool TechniqueState(Runtime* runtime, Technique technique, bool enabled, bool selected) {
        if (Writing == this) return false;
        std::lock_guard guard(Lock);
        Entry* entry = Find(runtime);
        if (!entry) return false;
        if (entry->RenderThread != std::this_thread::get_id()) {
            // No off-thread SDK query is allowed, but the event may change a
            // previously clean technique. Recheck it on the next render call.
            Invalidate(*entry);
            return false;
        }
        const int index = OwnedIndex(runtime, technique);
        if (index < 0 && index != -2) return false; // Other FX, including same-named ones, untouched.
        Invalidate(*entry);
        if (index == -2) return enabled; // Removed owned effects cannot reactivate.
        const uint32_t bit = 1u << index;
        if (enabled && selected) entry->Requested |= bit;
        else entry->Requested &= ~bit;
        // Disable immediately. Remember enables but defer them to the next
        // render callback, which checks the master and fresh depth binding.
        return enabled;
    }
};
} // namespace tq
