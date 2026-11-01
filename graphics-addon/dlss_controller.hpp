#pragma once
// The optional installed Feeder profile only. This controls input production,
// not arbitrary DLSS add-ons or unrelated user ReShade effects.
#include "effects_controller.hpp"
#include <vector>

namespace tq {
inline constexpr char DlssFeedFile[] = "DLSS5_Feed.fx";
inline constexpr char DlssFeedTechnique[] = "DLSS5_Feed";
inline constexpr char DlssMotionFile[] = "lumenite_Kernel.fx";
inline constexpr char DlssMotionTechnique[] = "Lumenite_Kernel";

struct DlssSnapshot {
    bool Requested = true, Available = false, DepthReady = false, Active = false, OrderReady = false;
};
inline const char* DlssStatus(const DlssSnapshot& state, bool selected, bool original) {
    if (!selected || !state.Requested) return "Off";
    if (original) return "Original";
    if (!state.Available) return "Shaders unavailable";
    if (!state.OrderReady) return "Shader order unavailable";
    if (!state.DepthReady) return "Waiting for depth";
    return state.Active ? "Feeding" : "Off";
}

template<class Runtime>
class DlssController {
    using Technique = decltype(std::declval<Runtime*>()->find_technique("", ""));
    struct Entry {
        Runtime* Identity = nullptr;
        std::thread::id Thread{};
        DlssSnapshot State{};
    };
    std::mutex Lock;
    std::array<Entry, MaxEffectRuntimes> Entries{};
    inline static thread_local const DlssController* Writing = nullptr;
    struct WriteScope {
        const DlssController* Previous;
        explicit WriteScope(const DlssController* value) : Previous(Writing) { Writing = value; }
        ~WriteScope() { Writing = Previous; }
    };
    Entry* Find(Runtime* runtime) {
        for (auto& entry : Entries) if (runtime && entry.Identity == runtime) return &entry;
        return nullptr;
    }
    Entry* Ensure(Runtime* runtime) {
        if (!runtime) return nullptr;
        if (auto* entry = Find(runtime)) return entry;
        for (auto& entry : Entries) if (!entry.Identity) {
            entry = {runtime, {}, {}};
            return &entry;
        }
        return nullptr;
    }
    static int Index(Runtime* runtime, Technique technique) {
        if (!runtime || !technique.handle) return -1;
        char file[256]{}, name[128]{};
        size_t a = sizeof(file), b = sizeof(name);
        runtime->get_technique_effect_name(technique, file, &a);
        runtime->get_technique_name(technique, name, &b);
        if (ExactEffectText(file, a, DlssMotionFile, sizeof(file)) &&
            ExactEffectText(name, b, DlssMotionTechnique, sizeof(name))) return 0;
        if (ExactEffectText(file, a, DlssFeedFile, sizeof(file)) &&
            ExactEffectText(name, b, DlssFeedTechnique, sizeof(name))) return 1;
        return -1;
    }
    static bool Order(Runtime* runtime, Technique motion, Technique feed) {
        // Keep every unrelated technique's relative order. Only move our motion
        // producer before our consumer, and never keep handles across callbacks.
        std::vector<Technique> list;
        bool overflow = false;
        runtime->enumerate_techniques(nullptr, [&](Runtime*, Technique value) {
            if (list.size() < 512) list.push_back(value); else overflow = true;
        });
        if (overflow) return false;
        size_t m = list.size(), f = list.size();
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].handle == motion.handle) m = i;
            if (list[i].handle == feed.handle) f = i;
        }
        if (m == list.size() || f == list.size()) return false;
        if (m > f) {
            list.erase(list.begin() + m);
            list.insert(list.begin() + f, motion);
            runtime->reorder_techniques(list.size(), list.data());
            // Another add-on can decline a reorder. Do not feed stale motion.
            list.clear();
            runtime->enumerate_techniques(nullptr, [&](Runtime*, Technique value) {
                if (list.size() < 512) list.push_back(value); else overflow = true;
            });
            m = list.size(); f = list.size();
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i].handle == motion.handle) m = i;
                if (list[i].handle == feed.handle) f = i;
            }
            if (overflow || m >= f || f == list.size()) return false;
        }
        return true;
    }
public:
    static bool IsOwned(Runtime* runtime, Technique technique) { return Index(runtime, technique) >= 0; }
    static bool IsFeed(Runtime* runtime, Technique technique) { return Index(runtime, technique) == 1; }
    void Reload(Runtime* runtime) {
        std::lock_guard guard(Lock);
        if (auto* entry = Ensure(runtime)) {
            const bool requested = entry->State.Requested;
            entry->State = {};
            entry->State.Requested = requested;
        }
    }
    void Forget(Runtime* runtime) {
        std::lock_guard guard(Lock);
        if (auto* entry = Find(runtime)) *entry = {};
    }
    void Clear() { std::lock_guard guard(Lock); Entries = {}; }
    DlssSnapshot Snapshot(Runtime* runtime) {
        std::lock_guard guard(Lock);
        if (auto* entry = Find(runtime)) return entry->State;
        return {};
    }
    void SetRequested(Runtime* runtime, bool requested) {
        std::lock_guard guard(Lock);
        if (auto* entry = Find(runtime)) entry->State.Requested = requested;
    }
    template<class DepthProbe>
    void Reconcile(Runtime* runtime, bool selected, bool original, DepthProbe&& probe) {
        // Config is immutable for this game session. An uninstalled profile
        // must not take over a user's separately installed Feeder or Kernel.
        if (!selected) return;
        std::lock_guard guard(Lock);
        auto* entry = Ensure(runtime);
        if (!entry) return;
        const auto thread = std::this_thread::get_id();
        if (entry->Thread == std::thread::id{}) entry->Thread = thread;
        if (entry->Thread != thread) return;
        const auto motion = runtime->find_technique(DlssMotionFile, DlssMotionTechnique);
        const auto feed = runtime->find_technique(DlssFeedFile, DlssFeedTechnique);
        const bool haveMotion = Index(runtime, motion) == 0, haveFeed = Index(runtime, feed) == 1;
        auto& state = entry->State;
        state.Available = haveMotion && haveFeed;
        const bool wanted = selected && !original && state.Requested;
        WriteScope ownWrite(this);
        state.OrderReady = wanted && state.Available && Order(runtime, motion, feed);
        state.DepthReady = state.OrderReady && probe(runtime);
        const bool enabled = wanted && state.Available && state.OrderReady && state.DepthReady;
        if (!enabled) {
            if (haveFeed && runtime->get_technique_state(feed)) runtime->set_technique_state(feed, false);
            if (haveMotion && runtime->get_technique_state(motion)) runtime->set_technique_state(motion, false);
        } else {
            if (!runtime->get_technique_state(motion)) runtime->set_technique_state(motion, true);
            const bool motionOn = runtime->get_technique_state(motion);
            if (runtime->get_technique_state(feed) != motionOn) runtime->set_technique_state(feed, motionOn);
            if (!runtime->get_technique_state(feed) && motionOn) runtime->set_technique_state(motion, false);
        }
        state.Active = enabled && runtime->get_technique_state(feed) && runtime->get_technique_state(motion);
    }
    bool TechniqueState(Runtime* runtime, Technique technique, bool enabled, bool selected) {
        if (Writing == this) return false;
        std::lock_guard guard(Lock);
        auto* entry = Find(runtime);
        if (!entry || entry->Thread != std::this_thread::get_id() || Index(runtime, technique) < 0) return false;
        entry->State.Requested = enabled && selected;
        return enabled; // An enable waits for the next coherent render callback.
    }
};
} // namespace tq
