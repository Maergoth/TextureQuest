#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

// Conservative side-plane culling for the existing 90-degree cube faces.
// This only removes boxes wholly outside a face. It does not change map size,
// filtering, near/far planes, caster membership or the map-update schedule.
namespace shadow_face_culling
{
using Point = std::array<float, 3>;
constexpr std::uint8_t AllFaces = 0x3f;
inline std::uint8_t ConservativeMask(const Point &low, const Point &high, const Point &light)
{
    double magnitude = 1;
    std::array<double, 3> relativeLow{}, relativeHigh{};
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(low[axis]) || !std::isfinite(high[axis]) || !std::isfinite(light[axis])
            || low[axis] > high[axis]) return AllFaces;
        magnitude = std::max({magnitude, std::abs(double(low[axis])), std::abs(double(high[axis])),
            std::abs(double(light[axis]))});
        relativeLow[axis] = double(low[axis]) - light[axis];
        relativeHigh[axis] = double(high[axis]) - light[axis];
    }
    // Expand rather than contract the face for float world transforms and the
    // rounded 90-degree projection. Use absolute-coordinate scale so distant
    // world origins remain conservative too. Touching/ambiguous boxes are kept.
    const double margin = magnitude * (64 * std::numeric_limits<float>::epsilon());
    std::uint8_t mask = 0;
    for (unsigned face = 0; face < 6; ++face)
    {
        const unsigned axis = face / 2;
        const double along = face % 2 ? -relativeLow[axis] : relativeHigh[axis];
        bool visible = true;
        for (unsigned across = 0; across < 3; ++across)
        {
            if (across == axis) continue;
            // The maxima of along+across and along-across over this AABB.
            // A negative maximum puts the whole box outside that side plane.
            if (along + relativeHigh[across] < -margin || along - relativeLow[across] < -margin)
            { visible = false; break; }
        }
        if (visible) mask |= std::uint8_t(1u << face);
    }
    return mask;
}
}
