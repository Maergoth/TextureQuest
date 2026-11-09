// Only retained, confirmed native painted-world shader pairs may be attenuated.
// Camera/caster measurement is optional and must not change the retained share.
#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace painted_emission_policy
{
struct Pair { uint64_t Vertex, Pixel; };
constexpr Pair Profiles[] = {
    { 0xd9379a14dac0f2edull, 0x7a8c2bd9056f65beull },
    { 0x3e5b529d0c5a3d7bull, 0x224654a253bd185full },
    { 0x5cc9b99698265066ull, 0x5887eab75424f1c7ull }
};
constexpr bool Vertex(uint64_t hash)
{
    for (const auto &pair : Profiles) if (pair.Vertex == hash) return true;
    return false;
}
constexpr bool Match(uint64_t vertex, uint64_t pixel)
{
    for (const auto &pair : Profiles) if (pair.Vertex == vertex && pair.Pixel == pixel) return true;
    return false;
}
inline float Kept(float value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.5f; }
}
