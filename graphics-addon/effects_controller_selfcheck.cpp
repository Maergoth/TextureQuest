// CPU-only contract check: no ReShade, graphics device, game or GPU is used.
#include "effects_controller.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
struct Handle { uint64_t handle = 0; };

struct FakeRuntime {
    bool Alive = true, Loaded = true, Enabled = false, UnrelatedEnabled = true;
    uint64_t Current = 1;
    unsigned Finds = 0, Reads = 0, Writes = 0;
    std::string File = "TQEffects.fx", Name = "TQBloom";

    void Validate(Handle value) const {
        if (!Alive || !Loaded || value.handle != Current)
            throw std::runtime_error("stale, unrelated or dead runtime handle used");
    }
    Handle find_technique(const char* file, const char* name) {
        if (!Alive || std::string(file) != "TQEffects.fx" || std::string(name) != "TQBloom")
            throw std::runtime_error("non-owned lookup or dead runtime used");
        ++Finds;
        return {Loaded ? Current : 0};
    }
    static void Text(const std::string& value, char* out, size_t* size) {
        const size_t capacity = *size;
        if (capacity) {
            const size_t copy = std::min(value.size(), capacity - 1);
            std::copy_n(value.data(), copy, out);
            out[copy] = '\0';
        }
        *size = value.size() + 1;
    }
    void get_technique_effect_name(Handle value, char* out, size_t* size) const {
        Validate(value);
        Text(File, out, size);
    }
    void get_technique_name(Handle value, char* out, size_t* size) const {
        Validate(value);
        Text(Name, out, size);
    }
    bool get_technique_state(Handle value) {
        Validate(value);
        ++Reads;
        return Enabled;
    }
    void set_technique_state(Handle value, bool enabled) {
        Validate(value);
        ++Writes;
        Enabled = enabled;
    }
};

unsigned Checks = 0, Failures = 0;
void Check(bool condition, const char* description) {
    ++Checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition)
        ++Failures;
}
}

int main() {
    try {
        Check(!tq::EffectsEnabled(false, false) && !tq::EffectsEnabled(false, true) &&
              !tq::EffectsEnabled(true, true) && tq::EffectsEnabled(true, false),
              "selection and Original master truth table");

        tq::EffectsController<FakeRuntime> controller;
        FakeRuntime main;
        controller.Reload(&main, true);
        Check(main.Enabled && main.Finds == 1 && main.Writes == 1, "startup resolves and enables owned technique");
        for (unsigned frame = 0; frame < 100; ++frame)
            controller.Reconcile(&main, true);
        Check(main.Finds == 1 && main.Writes == 1, "steady frames use cache and avoid repeated state writes");
        controller.Reconcile(&main, false);
        Check(!main.Enabled && main.Writes == 2, "Original master disables owned technique");
        controller.Reconcile(&main, true);
        Check(main.Enabled && main.Writes == 3, "Upscaled master enables owned technique again");
        Check(main.UnrelatedEnabled, "unrelated user technique stays enabled");

        // All old handles become invalid before the reload callback.
        main.Current = 2;
        main.Enabled = true;
        controller.Reload(&main, false);
        Check(!main.Enabled && main.Finds == 2, "reload discards stale handle before disabling newly loaded technique");
        main.Enabled = true; // Simulated preset/overlay change while Original.
        controller.Reconcile(&main, false);
        Check(!main.Enabled, "per-frame reconciliation enforces master after preset change");

        FakeRuntime compiling;
        compiling.Loaded = false;
        controller.Reload(&compiling, true);
        controller.Reconcile(&compiling, true);
        Check(compiling.Finds == 1 && compiling.Reads == 0, "missing technique is inert without a search loop");
        compiling.Loaded = true;
        controller.Reload(&compiling, true);
        Check(compiling.Enabled && compiling.Finds == 2, "asynchronous FX completion resolves at reload");

        FakeRuntime late;
        controller.Reconcile(&late, false);
        Check(late.Finds == 1 && !late.Enabled, "already alive runtime is discovered on first frame");
        FakeRuntime second;
        controller.Reload(&second, true);
        controller.Reconcile(&main, true);
        controller.Reconcile(&second, false);
        Check(main.Enabled && !second.Enabled, "runtime technique handles are isolated");

        FakeRuntime impostor_file;
        impostor_file.File = "UserEffects.fx";
        controller.Reload(&impostor_file, true);
        Check(impostor_file.Reads == 0 && impostor_file.Writes == 0, "same technique name from another file is refused");
        FakeRuntime impostor_name;
        impostor_name.Name = "UserBloom";
        controller.Reload(&impostor_name, true);
        Check(impostor_name.Reads == 0 && impostor_name.Writes == 0, "wrong technique name is refused");
        FakeRuntime oversized;
        oversized.File.assign(600, 'x');
        controller.Reload(&oversized, true);
        Check(oversized.Reads == 0 && oversized.Writes == 0, "truncated or oversized effect name is refused");

        controller.Forget(&main);
        main.Alive = false;
        const auto stopped_writes = main.Writes;
        controller.DisableAndClear();
        Check(main.Writes == stopped_writes, "destroyed runtime is forgotten without API calls");
        Check(!compiling.Enabled && !late.Enabled && !second.Enabled, "orderly unload disables all live owned techniques");
        Check(compiling.UnrelatedEnabled && late.UnrelatedEnabled && second.UnrelatedEnabled,
              "orderly unload preserves unrelated techniques");

        // A reset can reuse the same runtime address with different handles.
        main.Alive = true;
        main.Current = 3;
        main.Enabled = false;
        controller.Reload(&main, true);
        Check(main.Enabled && main.Finds == 3, "reset runtime address resolves a new handle");
        controller.Forget(&main);
        main.Alive = false;
        controller.Reload(nullptr, true);
        controller.Reconcile(nullptr, false);
        controller.Forget(nullptr);
        controller.DisableAndClear();
        Check(true, "null callbacks and repeated clear are inert");
    } catch (const std::exception& error) {
        ++Failures;
        std::printf("FAIL exception: %s\n", error.what());
    }
    std::printf("effects controller: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
