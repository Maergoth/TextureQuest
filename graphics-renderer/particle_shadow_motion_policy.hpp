#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// This budget applies only to additional particle-map motion refinements.
// New maps, changed slots/ranges and the existing >0.1-unit motion rule must
// run first and remain outside this budget. A committed map always owns the
// same source pose used by lighting and moving-character shadows.
namespace particle_shadow_motion_policy
{
constexpr float RefinementDistance = 0.005f;
constexpr float MandatoryDistance = 0.1f;
constexpr float EnvelopePadding = 0.5f;
constexpr uint32_t ExtraCubeLimit = 8;
constexpr uint32_t ExtraFacePixelUnits = 64;
constexpr uint32_t ReferenceFaceSize = 256;
constexpr uint32_t MaximumFaceSize = 4096;

struct Candidate
{
    uint64_t key = 0;
    uint64_t builtFrame = 0;
    float displacement = 0;
    uint32_t faceSize = 0;
};

inline uint32_t FacePixelUnits(uint32_t faceSize)
{
    if (!faceSize || faceSize > MaximumFaceSize)
        return 0;
    constexpr uint64_t unit = uint64_t(ReferenceFaceSize) * ReferenceFaceSize;
    return uint32_t((uint64_t(faceSize) * faceSize + unit - 1) / unit);
}

inline bool IsRefinementCandidate(const Candidate &candidate, uint64_t frame)
{
    return candidate.builtFrame && candidate.builtFrame < frame
        && std::isfinite(candidate.displacement)
        && candidate.displacement >= RefinementDistance
        && candidate.displacement <= MandatoryDistance
        && FacePixelUnits(candidate.faceSize) != 0;
}

inline std::vector<uint64_t> SelectRefinements(const std::vector<Candidate> &candidates, uint64_t frame)
{
    std::vector<Candidate> ordered;
    ordered.reserve(candidates.size());
    for (const auto &candidate : candidates)
        if (IsRefinementCandidate(candidate, frame))
            ordered.push_back(candidate);

    // Oldest committed pose first gives bounded service to persistent requests
    // without a noisy or bright source monopolizing refinements. Error and key
    // break ties independently of unordered-map iteration order.
    std::sort(ordered.begin(), ordered.end(), [](const Candidate &a, const Candidate &b) {
        if (a.builtFrame != b.builtFrame)
            return a.builtFrame < b.builtFrame;
        if (a.displacement != b.displacement)
            return a.displacement > b.displacement;
        if (a.key != b.key)
            return a.key < b.key;
        return a.faceSize < b.faceSize;
    });

    std::vector<uint64_t> selected;
    selected.reserve(ExtraCubeLimit);
    uint32_t used = 0;
    for (const auto &candidate : ordered)
    {
        const uint32_t cost = FacePixelUnits(candidate.faceSize);
        if (cost > ExtraFacePixelUnits - used
            || std::find(selected.begin(), selected.end(), candidate.key) != selected.end())
            continue;
        selected.push_back(candidate.key);
        used += cost;
        if (selected.size() == ExtraCubeLimit)
            break;
    }
    return selected;
}

inline bool CoversEnvelope(float centreDistance, float requestedRadius, float envelopeRadius)
{
    // Evaluate in double: rounding the sum down in float could certify a sphere
    // that actually protrudes beyond the conservatively gathered envelope.
    return std::isfinite(centreDistance) && std::isfinite(requestedRadius)
        && std::isfinite(envelopeRadius) && centreDistance >= 0
        && requestedRadius >= 0 && envelopeRadius >= 0
        && double(centreDistance) + double(requestedRadius) <= double(envelopeRadius);
}
}
