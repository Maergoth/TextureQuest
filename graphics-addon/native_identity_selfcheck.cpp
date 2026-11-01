// Narrow CPU check of both real production name-check callers' shared predicate.
#include "effects_controller.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

namespace {
unsigned Checks = 0, Failures = 0;
void Check(bool result, const char* what) {
    ++Checks; std::printf("%s %s\n", result ? "PASS" : "FAIL", what);
    if (!result) ++Failures;
}
struct Handle { uint64_t handle = 0; };
struct NamesRuntime {
    bool IncludesNul = false, MissingNul = false;
    std::string File = "TQEffects.fx";
    uint32_t Enabled = 0;
    unsigned Writes = 0;
    Handle find_technique(const char*, const char* name) {
        for (size_t i = 0; i < tq::EffectNames.size(); ++i)
            if (std::string(name) == tq::EffectNames[i]) return {i + 1};
        return {};
    }
    void enumerate_techniques(const char*, void(*callback)(NamesRuntime*, Handle, void*), void* data) {
        for (size_t i = 0; i < tq::EffectNames.size(); ++i) callback(this, {i + 1}, data);
    }
    void Copy(const std::string& value, char* out, size_t* size) {
        const auto n = std::min(value.size(), *size - 1);
        std::copy_n(value.data(), n, out); out[n] = MissingNul ? '!' : 0;
        *size = n + (IncludesNul ? 1 : 0);
    }
    void get_technique_effect_name(Handle, char* out, size_t* size) { Copy(File, out, size); }
    void get_technique_name(Handle h, char* out, size_t* size) { Copy(tq::EffectNames[h.handle - 1], out, size); }
    bool get_technique_state(Handle h) const { return (Enabled & (1u << (h.handle - 1))) != 0; }
    void set_technique_state(Handle h, bool on) {
        ++Writes;
        if (on) Enabled |= 1u << (h.handle - 1);
        else Enabled &= ~(1u << (h.handle - 1));
    }
};
}
int main() {
    const char* names[] = {"TQEffects.fx", "TQDepthReady", "TQDiagnosticDepth", "bufready_depth"};
    for (const char* name : names) {
        const auto n = std::strlen(name);
        Check(tq::ExactEffectText(name, n, name, n + 1), "exact copied-character count accepted");
        Check(tq::ExactEffectText(name, n + 1, name, n + 1), "exact including-NUL count accepted");
    }
    const std::string name = "TQEffects.fx", longer = name + ".extra";
    const auto n = name.size();
    Check(!tq::ExactEffectText(longer.c_str(), n, name.c_str(), longer.size() + 1), "matching prefix with forged copied count rejected");
    Check(!tq::ExactEffectText(longer.c_str(), n + 1, name.c_str(), longer.size() + 1), "matching prefix with forged NUL count rejected");
    std::string missing = name + '!';
    Check(!tq::ExactEffectText(missing.data(), n, name.c_str(), missing.size()), "missing NUL rejected for copied count");
    Check(!tq::ExactEffectText(missing.data(), n + 1, name.c_str(), missing.size()), "missing NUL rejected for including-NUL count");
    Check(!tq::ExactEffectText(name.c_str(), n, name.c_str(), n), "insufficient NUL capacity rejected before compare");
    Check(!tq::ExactEffectText(name.c_str(), n - 1, name.c_str(), n + 1), "truncated report rejected");
    Check(!tq::ExactEffectText(name.c_str(), n + 2, name.c_str(), n + 1), "oversized report rejected");
    Check(!tq::ExactEffectText(nullptr, n, name.c_str(), n + 1), "null text rejected");
    for (const bool includes : {false, true}) {
        tq::EffectsController<NamesRuntime> controller; NamesRuntime runtime; runtime.IncludesNul = includes;
        controller.Reload(&runtime);
        controller.Reconcile(&runtime, true, [](auto*) { return true; });
        Check(runtime.Enabled == tq::DefaultEffectRequests && runtime.Writes == 2,
              "actual production controller accepts exact owned names with either SDK style");
    }
    for (const bool missingNul : {false, true}) {
        tq::EffectsController<NamesRuntime> controller; NamesRuntime runtime;
        runtime.MissingNul = missingNul; if (!missingNul) runtime.File += ".extra";
        controller.Reload(&runtime);
        controller.Reconcile(&runtime, true, [](auto*) { return true; });
        Check(runtime.Enabled == 0 && runtime.Writes == 0, "actual controller refuses prefix or unterminated effect identity");
    }
    std::printf("name-size compatibility: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
