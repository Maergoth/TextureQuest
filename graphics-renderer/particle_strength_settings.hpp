#pragma once
#include <algorithm>
#include <cmath>

namespace particle_strength_settings
{
constexpr const char *CurrentKey = "ParticleLightStrengthV3";
constexpr const char *PreviousKey = "ParticleLightStrengthV2";
constexpr const char *LegacyKey = "ParticleLightStrength";
constexpr const char *UnitsKey = "ParticleLightStrengthUnits";
constexpr int Version = 3;
constexpr float Reference = 2.5f;
constexpr float PreviousReference = 5.0f;
constexpr float Default = 1.0f;
constexpr float LegacyMaximum = 128.0f;
constexpr float Maximum = LegacyMaximum / Reference;
constexpr float PreviousMaximum = LegacyMaximum / PreviousReference;

inline float Clean(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, Maximum) : Default;
}

inline float FromPrevious(float value)
{
    // V2 used a 5x reference. Preserve its effective clamp and invalid-value
    // fallback before converting: saved V2 0.5x displays as V3 1x.
    const float previous = std::isfinite(value) ? std::clamp(value, 0.0f, PreviousMaximum) : 1.0f;
    return previous * (PreviousReference / Reference);
}

inline float FromLegacy(float value)
{
    // Match the old clamp/fallback before changing units. A previously disabled
    // light remains disabled; original 2.5x becomes new 1x at the same gain.
    const float previous = std::isfinite(value) ? std::clamp(value, 0.0f, LegacyMaximum) : 1.0f;
    return previous / Reference;
}

template<class Get, class Set> float Load(Get get, Set set)
{
    float current = Default, previous = 1.0f, legacy = 1.0f;
    const bool modern = get(CurrentKey, current);
    const bool prior = !modern && get(PreviousKey, previous);
    const bool old = !modern && !prior && get(LegacyKey, legacy);
    const float value = modern ? Clean(current) : prior ? FromPrevious(previous) : old ? FromLegacy(legacy) : Default;
    int version = 0;
    get(UnitsKey, version);
    // The new value is authoritative even if its marker was not saved. The
    // immutable prior keys make an interrupted migration safe to retry. Publish
    // the value before its marker; a marker alone never skips conversion.
    if (!modern || current != value) set(CurrentKey, value);
    if (version < Version) set(UnitsKey, Version);
    return value;
}

template<class Set> void Store(float value, Set set)
{
    set(CurrentKey, Clean(value));
    set(UnitsKey, Version);
}
}
