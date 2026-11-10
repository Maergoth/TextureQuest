#pragma once
#include <algorithm>
#include <cmath>

namespace particle_strength_settings
{
constexpr const char *CurrentKey = "ParticleLightStrengthV2";
constexpr const char *LegacyKey = "ParticleLightStrength";
constexpr const char *UnitsKey = "ParticleLightStrengthUnits";
constexpr int Version = 2;
constexpr float Reference = 5.0f;
constexpr float Default = 1.0f;
constexpr float LegacyMaximum = 128.0f;
constexpr float Maximum = LegacyMaximum / Reference;

inline float Clean(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, Maximum) : Default;
}

inline float FromLegacy(float value)
{
    // Match the old clamp/fallback before changing units. A previously disabled
    // light remains disabled; old 5x becomes new 1x at the same physical gain.
    const float previous = std::isfinite(value) ? std::clamp(value, 0.0f, LegacyMaximum) : 1.0f;
    return previous / Reference;
}

template<class Get, class Set> float Load(Get get, Set set)
{
    float current = Default, legacy = 1.0f;
    const bool modern = get(CurrentKey, current);
    const bool old = !modern && get(LegacyKey, legacy);
    const float value = modern ? Clean(current) : old ? FromLegacy(legacy) : Default;
    int version = 0;
    get(UnitsKey, version);
    // The new value is authoritative even if its marker was not saved. The
    // immutable old key makes an interrupted migration safe to retry.
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
