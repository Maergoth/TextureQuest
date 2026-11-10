// Spatial object emission policy; independent of DirectX so conservation can be checked on the CPU.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace object_radiance_policy
{
// A first sparse observation may establish a local cell. Repeated sparse views do not acquire confidence by count or
// replace an established value: a replacement must have the full per-observation support, including measured black.
inline bool ReplaceObservation(bool known, unsigned texels, unsigned minimum)
{
    return texels > 0 && (!known || texels >= minimum);
}

// Geometry arrives asynchronously. Sampling before it exists would consume the normal cooldown without placing
// any radiance, leaving a fresh emitter dark for an entire interval after its shape becomes ready.
inline bool MeasurementReady(bool shapeKnown, double area, uint64_t lastFrame, uint64_t frame, uint64_t interval)
{
    return shapeKnown && std::isfinite(area) && area > 0
        && (!lastFrame || (frame >= lastFrame && frame - lastFrame >= interval));
}

inline bool NewerMeasurement(uint64_t sequence, uint64_t applied)
{
    return sequence != 0 && sequence > applied;
}

inline std::array<float, 4> Observation(double red, double green, double blue, unsigned texels)
{
    if (!texels)
        return {};
    const double mean[3] = { red / texels, green / texels, blue / texels };
    for (double channel : mean)
        if (!std::isfinite(channel) || channel < 0 || channel > std::numeric_limits<float>::max())
            return {};
    return { static_cast<float>(mean[0]), static_cast<float>(mean[1]), static_cast<float>(mean[2]), 1 };
}

// Only for a missing nominal geometry cell: tolerate tiny reprojection error across a grid face, edge or corner.
// The callback tests geometry existence, never colour/coverage. Exactly one nearby candidate must exist; ambiguous
// neighbours fail closed. This does not fill holes caused by coarse geometry sampling away from a grid boundary.
template<class Exists>
inline bool FindBoundaryNeighbor(const float *point, float width, int *cell, Exists exists)
{
    if (!std::isfinite(width) || width <= 0)
        return false;
    struct Axis
    {
        int Value[3] = {};
        double Gap[3] = {};
        unsigned Count = 1;
    };
    Axis axes[3];
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(point[axis]))
            return false;
        const double coordinate = point[axis];
        if (std::floor(coordinate / width) != cell[axis])
            return false;
        const double relative = 4 * std::numeric_limits<float>::epsilon()
            * std::max(std::abs(coordinate), static_cast<double>(width));
        const double tolerance = std::min({ 0.01, 0.005 * width, std::max(0.0001, relative) });
        const double lower = static_cast<double>(cell[axis]) * width;
        const double toLower = coordinate - lower, toUpper = lower + width - coordinate;
        Axis &options = axes[axis];
        options.Value[0] = cell[axis];
        if (toLower >= 0 && toLower <= tolerance && cell[axis] > std::numeric_limits<int>::min())
        {
            options.Value[options.Count] = cell[axis] - 1;
            options.Gap[options.Count++] = toLower / tolerance;
        }
        if (toUpper >= 0 && toUpper <= tolerance && cell[axis] < std::numeric_limits<int>::max())
        {
            options.Value[options.Count] = cell[axis] + 1;
            options.Gap[options.Count++] = toUpper / tolerance;
        }
    }
    int candidate[3] = {};
    bool found = false;
    for (unsigned x = 0; x < axes[0].Count; ++x)
        for (unsigned y = 0; y < axes[1].Count; ++y)
            for (unsigned z = 0; z < axes[2].Count; ++z)
            {
                if (!(x || y || z))
                    continue;
                const double distance = axes[0].Gap[x] * axes[0].Gap[x] + axes[1].Gap[y] * axes[1].Gap[y]
                    + axes[2].Gap[z] * axes[2].Gap[z];
                if (distance > 1 || !exists(axes[0].Value[x], axes[1].Value[y], axes[2].Value[z]))
                    continue;
                if (found)
                    return false;
                found = true;
                candidate[0] = axes[0].Value[x];
                candidate[1] = axes[1].Value[y];
                candidate[2] = axes[2].Value[z];
            }
    if (found)
        std::copy(candidate, candidate + 3, cell);
    return found;
}

// RGB is integrated over the known child's full geometric area. A parent stores that flux divided by its FULL area,
// not just its observed area. Unknown children contribute zero, and therefore cannot inherit a visible sibling's hue.
// The fourth component records some actual descendant coverage, including darkness; it is not a confidence counter.
class Flux
{
    double Sum[3] = {};
    bool Known = false;

public:
    void Add(double red, double green, double blue, bool known, double area)
    {
        if (!known || !std::isfinite(area) || area <= 0 || !std::isfinite(red) || !std::isfinite(green)
            || !std::isfinite(blue) || red < 0 || green < 0 || blue < 0)
            return;
        Sum[0] += red * area;
        Sum[1] += green * area;
        Sum[2] += blue * area;
        Known = true;
    }

    std::array<float, 4> Average(double fullArea) const
    {
        if (!Known || !std::isfinite(fullArea) || fullArea <= 0)
            return {};
        const double mean[3] = { Sum[0] / fullArea, Sum[1] / fullArea, Sum[2] / fullArea };
        for (double channel : mean)
            if (!std::isfinite(channel) || channel < 0 || channel > std::numeric_limits<float>::max())
                return {};
        return { static_cast<float>(mean[0]), static_cast<float>(mean[1]), static_cast<float>(mean[2]), 1 };
    }
};
}
