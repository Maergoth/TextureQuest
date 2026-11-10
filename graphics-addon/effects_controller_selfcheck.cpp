// CPU fake exercises the actual production controller, including reentrant
// SDK writes. No ReShade DLL, graphics device, game, hooks or GPU are used.
#include "effects_controller.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>

namespace {
struct Handle { uint64_t handle = 0; };
struct FakeRuntime {
    bool Alive = true, Loaded = true, Depth = true, Unrelated = true;
    bool RetiredLoaded = false, DuplicateCurrent = false, RejectDisable = false;
    uint64_t Generation = 1;
    uint32_t Enabled = 0;
    unsigned Calls = 0, Finds = 0, Writes = 0, Probes = 0, Enumerations = 0;
    int WrongName = -1;
    std::string File = "TQEffects.fx";
    std::function<bool(Handle, bool)> Event;
    void Live() { if (!Alive) throw std::runtime_error("dead runtime used"); ++Calls; }
    size_t Index(Handle h) {
        Live();
        const auto slot = h.handle % 100;
        if (h.handle / 100 != Generation || slot < 1 || slot > 7 ||
            (slot <= 3 && !Loaded) || (slot >= 4 && slot <= 6 && !RetiredLoaded) ||
            (slot == 7 && (!Loaded || !DuplicateCurrent)))
            throw std::runtime_error("stale, missing or unrelated handle used");
        return static_cast<size_t>(h.handle % 100 - 1);
    }
    Handle At(size_t i) const { return {Generation * 100 + i + 1}; }
    Handle find_technique(const char* file, const char* name) {
        Live(); ++Finds;
        if (std::string(file) != "TQEffects.fx") throw std::runtime_error("global lookup");
        for (size_t i = 0; i < tq::EffectNames.size(); ++i)
            if (std::string(name) == tq::EffectNames[i]) return Loaded ? At(i) : Handle{};
        for (size_t i = 0; i < tq::RetiredEffectNames.size(); ++i)
            if (std::string(name) == tq::RetiredEffectNames[i]) return RetiredLoaded ? At(i + 3) : Handle{};
        throw std::runtime_error("unowned technique lookup");
    }
    void enumerate_techniques(const char* file, void(*callback)(FakeRuntime*, Handle, void*), void* data) {
        Live(); ++Enumerations;
        if (std::string(file) != "TQEffects.fx") throw std::runtime_error("global enumeration");
        if (Loaded) for (size_t i = 0; i < tq::EffectNames.size(); ++i) callback(this, At(i), data);
        if (RetiredLoaded) for (size_t i = 0; i < tq::RetiredEffectNames.size(); ++i) callback(this, At(i + 3), data);
        if (Loaded && DuplicateCurrent) callback(this, At(6), data);
    }
    static void Text(const std::string& text, char* out, size_t* size) {
        const size_t n = *size ? std::min(*size - 1, text.size()) : 0;
        std::copy_n(text.data(), n, out);
        if (*size) out[n] = 0;
        *size = text.size() + 1;
    }
    void get_technique_effect_name(Handle h, char* out, size_t* size) { Index(h); Text(File, out, size); }
    void get_technique_name(Handle h, char* out, size_t* size) {
        const auto i = Index(h);
        Text(static_cast<int>(i) == WrongName ? "UserEffect" :
            (i < 3 ? tq::EffectNames[i] : (i < 6 ? tq::RetiredEffectNames[i - 3] : tq::EffectNames[0])), out, size);
    }
    bool get_technique_state(Handle h) { return (Enabled & (1u << Index(h))) != 0; }
    void set_technique_state(Handle h, bool enabled) {
        const size_t i = Index(h); ++Writes;
        if (Event && Event(h, enabled)) return;
        if (!enabled && RejectDisable) return;
        if (enabled) Enabled |= 1u << i;
        else Enabled &= ~(1u << i);
    }
    void Menu(size_t i, bool enabled) {
        const auto h = At(i);
        if (!Event || !Event(h, enabled)) {
            if (enabled) Enabled |= 1u << i;
            else Enabled &= ~(1u << i);
        }
    }
};
using Controller = tq::EffectsController<FakeRuntime>;
auto Probe = [](FakeRuntime* runtime) { runtime->Live(); ++runtime->Probes; return runtime->Depth; };
void Attach(Controller& controller, FakeRuntime& runtime, bool selected = true) {
    runtime.Event = [&controller, &runtime, selected](Handle h, bool on) {
        return controller.TechniqueState(&runtime, h, on, selected);
    };
}
unsigned Checks = 0, Failures = 0;
void Check(bool value, const char* description) {
    ++Checks;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
    if (!value) ++Failures;
}
}
int main() {
    try {
        Check(!tq::EffectsEnabled(false, false) && !tq::EffectsEnabled(false, true) &&
              !tq::EffectsEnabled(true, true) && tq::EffectsEnabled(true, false), "selection/master truth table");
        Controller controller;
        FakeRuntime main; Attach(controller, main);
        controller.Reload(&main);
        Check(main.Calls == 0, "init/reload performs no SDK calls");
        controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == tq::DefaultEffectRequests && main.Writes == 2, "AO and contact shadows initially on; debug opt-in");
        const auto writes = main.Writes;
        for (unsigned frame = 0; frame < 20; ++frame) controller.Reconcile(&main, true, Probe);
        Check(main.Writes == writes && main.Probes == 21, "steady frame uses fresh probe without repeated writes");
        main.Menu(1, false); // Contact shadows.
        controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == 0x01 && main.Writes == writes, "menu disable stays off instead of forced contact shadow enable");
        main.Menu(2, true); // Debug enable is deferred to a guarded render callback.
        Check(!(main.Enabled & 0x04), "menu enable deferred before depth/master guard");
        controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == 0x05, "menu enables debug after fresh depth guard");
        main.Menu(0, false);
        controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == 0x04, "AO/contact/debug have independent menu choices");
        controller.Reconcile(&main, false, Probe);
        Check(main.Enabled == 0, "Original disables all owned effects");
        controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == 0x04, "Upscaled restores exact individual choices, not defaults");
        controller.Reconcile(&main, false, Probe);
        main.Menu(1, true);
        controller.Reconcile(&main, false, Probe);
        controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == 0x06, "enable requested while Original is remembered for Upscaled");
        main.Depth = false;
        controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == 0 && main.Unrelated, "invalid depth disables depth effects only; unrelated effects remain");
        main.Depth = true;
        controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == 0x06, "depth recovery restores prior depth choices");
        ++main.Generation; main.Enabled = 0;
        controller.Reload(&main);
        controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == 0x06, "FX reload preserves menu requests and uses fresh handles");
        const auto calls = main.Calls;
        bool foreignFrame = true, foreignEvent = true;
        std::thread foreign([&] {
            controller.Reload(&main); // Metadata-only even on a different thread.
            foreignFrame = controller.Reconcile(&main, true, Probe);
            foreignEvent = controller.TechniqueState(&main, main.At(0), false, true);
        }); foreign.join();
        Check(!foreignFrame && !foreignEvent && main.Calls == calls, "wrong thread cannot query or write SDK state");
        FakeRuntime second; Attach(controller, second);
        controller.Reconcile(&second, true, Probe);
        second.Menu(1, false);
        controller.Reconcile(&second, true, Probe);
        controller.Reconcile(&main, true, Probe);
        Check(second.Enabled == 0x01 && main.Enabled == 0x06, "two runtimes never share choices or technique handles");
        FakeRuntime compiling; Attach(controller, compiling); compiling.Loaded = false;
        controller.Reload(&compiling); controller.Reconcile(&compiling, true, Probe);
        Check(compiling.Enabled == 0 && compiling.Writes == 0, "missing asynchronously compiling FX inert");
        compiling.Loaded = true; ++compiling.Generation;
        controller.Reload(&compiling); controller.Reconcile(&compiling, true, Probe);
        Check(compiling.Enabled == 0x03, "later reload discovers compiled FX");
        FakeRuntime impostor; Attach(controller, impostor); impostor.File = "UserEffects.fx";
        controller.Reconcile(&impostor, true, Probe);
        Check(impostor.Writes == 0 && impostor.Enabled == 0, "same names from another effect file are refused");
        FakeRuntime excess;
        Check(!controller.Reconcile(&excess, true, Probe) && excess.Calls == 0, "bounded fifth runtime gets zero SDK calls");
        controller.Forget(&impostor);
        impostor.File.assign(600, 'x');
        controller.Reload(&impostor); controller.Reconcile(&impostor, true, Probe);
        Check(impostor.Writes == 0, "oversized/truncated file identity refused");
        controller.Forget(&impostor);
        impostor.File = "TQEffects.fx"; impostor.WrongName = 0;
        controller.Reload(&impostor); controller.Reconcile(&impostor, true, Probe);
        Check(impostor.Enabled == 0x02, "wrong technique identity skipped without touching others");
        const auto forgetCalls = main.Calls;
        controller.Forget(&main); main.Alive = false;
        controller.Clear();
        Check(main.Calls == forgetCalls, "destroy and unload clear metadata without dead/off-thread SDK calls");
        main.Alive = true; ++main.Generation; main.Enabled = 0;
        controller.Reload(&main); controller.Reconcile(&main, true, Probe);
        Check(main.Enabled == tq::DefaultEffectRequests, "reused destroyed runtime address starts fresh");
        controller.Clear();
        FakeRuntime unselected; Attach(controller, unselected, false);
        controller.Reconcile(&unselected, false, Probe);
        unselected.Menu(1, true);
        controller.Reconcile(&unselected, false, Probe);
        Check(unselected.Enabled == 0 && unselected.Probes == 0, "unselected effects cannot enable from menu or probe depth");
        const auto idleCalls = unselected.Calls;
        const auto idleFinds = unselected.Finds;
        for (unsigned frame = 0; frame < 40; ++frame) controller.Reconcile(&unselected, false, Probe);
        Check(unselected.Calls == idleCalls && unselected.Finds == idleFinds && unselected.Probes == 0,
              "clean unselected effect phases perform zero SDK lookups, enumeration or depth queries");
        controller.Clear();
        FakeRuntime absent; Attach(controller, absent); absent.Loaded = false;
        controller.Reconcile(&absent, true, Probe);
        const auto absentCalls = absent.Calls;
        for (unsigned frame = 0; frame < 40; ++frame) controller.Reconcile(&absent, true, Probe);
        Check(absent.Calls == absentCalls, "known absent owned effects make subsequent active-master phases inert");
        controller.SetRequested(&absent, 2, true);
        controller.Reconcile(&absent, true, Probe);
        Check(absent.Calls > absentCalls, "request changes invalidate absence facts");
        absent.Loaded = true; ++absent.Generation;
        controller.Reload(&absent); controller.Reconcile(&absent, true, Probe);
        Check(absent.Enabled == 0x07, "reload after cached absence discovers current handles and restores requests");
        const auto activeProbes = absent.Probes;
        const auto activeFinds = absent.Finds;
        absent.Depth = false; controller.Reconcile(&absent, true, Probe);
        absent.Depth = true; controller.Reconcile(&absent, true, Probe);
        Check(absent.Probes == activeProbes + 2 && absent.Finds == activeFinds + 6 && absent.Enabled == 0x07,
              "active effects still refresh three handles and depth each phase while retired absence skips lookups");
        for (size_t i = 0; i < tq::EffectNames.size(); ++i) controller.SetRequested(&absent, i, false);
        controller.Reconcile(&absent, true, Probe);
        const auto noRequestCalls = absent.Calls;
        controller.Reconcile(&absent, true, Probe);
        Check(absent.Enabled == 0 && absent.Calls == noRequestCalls, "zero requested effects enter verified idle without depth polling");
        absent.Menu(0, true); controller.Reconcile(&absent, true, Probe);
        Check(absent.Enabled == 0x01, "owned technique enable invalidates clean idle and resumes guarded activation");
        controller.Reconcile(&absent, false, Probe);
        const auto masterOffCalls = absent.Calls;
        controller.Reconcile(&absent, false, Probe);
        controller.Reconcile(&absent, true, Probe);
        Check(absent.Enabled == 0x01 && absent.Calls > masterOffCalls, "master edge invalidates idle and restores requested effect");
        controller.Clear();
        FakeRuntime stale; Attach(controller, stale, false);
        stale.RetiredLoaded = true; stale.DuplicateCurrent = true; stale.Enabled = 0x7f;
        controller.Reconcile(&stale, false, Probe);
        const auto cleanStaleCalls = stale.Calls;
        controller.Reconcile(&stale, false, Probe);
        Check(stale.Enabled == 0 && stale.Unrelated && stale.Calls == cleanStaleCalls,
              "idle sweep disables every current, retired and duplicate owned technique then skips clean phases");
        stale.Menu(3, true);
        const auto retiredEventCalls = stale.Calls;
        controller.Reconcile(&stale, false, Probe);
        Check(stale.Enabled == 0 && stale.Calls > retiredEventCalls, "retired menu enable remains blocked and invalidates cleanup facts");
        ++stale.Generation; stale.Enabled = 0x7f;
        controller.Reload(&stale); controller.Reconcile(&stale, false, Probe);
        Check(stale.Enabled == 0, "reload while idle suppresses newly enabled cached owned effects using new handles");
        stale.Enabled = 1; stale.RejectDisable = true; controller.Reload(&stale);
        controller.Reconcile(&stale, false, Probe);
        const auto rejectedCalls = stale.Calls;
        controller.Reconcile(&stale, false, Probe);
        Check(stale.Enabled == 1 && stale.Calls > rejectedCalls, "failed disable never establishes a false clean-idle cache");
        stale.RejectDisable = false; controller.Reconcile(&stale, false, Probe);
        Check(stale.Enabled == 0, "later successful disable completes idle cleanup");
        const auto beforeForeignEvent = stale.Calls;
        std::thread foreignToggle([&] { stale.Menu(0, true); }); foreignToggle.join();
        Check(stale.Calls == beforeForeignEvent && stale.Enabled == 1, "foreign technique event only invalidates metadata without SDK queries");
        controller.Reconcile(&stale, false, Probe);
        Check(stale.Enabled == 0, "render reconciliation repairs a foreign-thread change after idle cache invalidation");
        stale.File = "UserEffects.fx"; stale.Enabled = 0x7f; controller.Reload(&stale);
        controller.Reconcile(&stale, false, Probe);
        Check(stale.Enabled == 0x7f, "idle sweep leaves techniques from an unrelated file untouched");
        controller.Clear();
        Check(!controller.Reconcile(nullptr, true, Probe), "null runtime is inert");
        controller.Reload(nullptr); controller.Forget(nullptr); controller.Clear();
    } catch (const std::exception& error) {
        ++Failures; std::printf("FAIL exception: %s\n", error.what());
    }
    std::printf("combined effects controller: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
