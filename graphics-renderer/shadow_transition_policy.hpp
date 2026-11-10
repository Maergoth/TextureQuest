#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// CPU policy shared by the optional map history and its bounded fixture.
namespace shadow_transition_policy
{
constexpr double DurationSeconds = .150;
constexpr unsigned MaximumTransitions = 6;
constexpr float MaximumDisplacement = 2.f;
constexpr float MaximumParticleCore = .35f;
struct State
{
    uint64_t Owner = 0, Epoch = 0;
    int Slot = -1;
    double Started = 0;
    bool Active = false;
};
struct Source
{
    std::array<float, 3> Position{}, Normal{};
    float Radius = 0, Core = 0, Range = 0, Area = 0;
    uint64_t Emitter = 0;
    bool Particle = false;
};
inline bool Finite(const Source &source)
{
    for (float value : source.Position) if (!std::isfinite(value)) return false;
    for (float value : source.Normal) if (!std::isfinite(value)) return false;
    return std::isfinite(source.Radius) && source.Radius > .05f && std::isfinite(source.Core) && source.Core >= 0
        && source.Core <= MaximumParticleCore && (source.Particle || source.Core == 0)
        && std::isfinite(source.Range) && source.Range > 0 && std::isfinite(source.Area) && source.Area > 0;
}
inline bool SameShape(const Source &a, const Source &b)
{
    return Finite(a) && Finite(b) && a.Normal == b.Normal && a.Radius == b.Radius && a.Core == b.Core
        && a.Range == b.Range && a.Area == b.Area && a.Emitter == b.Emitter && a.Particle == b.Particle;
}
inline bool CompatibleEndpoints(const Source &a, const Source &b)
{
    // Core exclusion is baked independently into each exact retained depth cube.
    // A changing particle core need not invalidate a correctly paired handoff.
    return Finite(a) && Finite(b) && a.Normal == b.Normal && a.Radius == b.Radius
        && a.Range == b.Range && a.Area == b.Area && a.Emitter == b.Emitter && a.Particle == b.Particle;
}
inline bool BoundedMove(const Source &a, const Source &b)
{
    if (!CompatibleEndpoints(a, b)) return false;
    float squared = 0;
    for (unsigned i = 0; i < 3; ++i) { const float delta = a.Position[i] - b.Position[i]; squared += delta * delta; }
    return std::isfinite(squared) && squared <= MaximumDisplacement * MaximumDisplacement;
}
inline bool SamePose(const Source &a, const Source &b)
{
    return SameShape(a, b) && a.Position == b.Position;
}
inline bool Live(const State &state, uint64_t owner, int slot, uint64_t epoch, double now)
{
    return state.Active && state.Owner == owner && state.Slot == slot && state.Epoch == epoch && std::isfinite(now)
        && std::isfinite(state.Started) && now >= state.Started && now - state.Started < DurationSeconds;
}
inline bool Start(State &state, uint64_t owner, int slot, uint64_t epoch, double now)
{
    // Coalesced writes keep the first endpoint and its original deadline.
    if (state.Active && std::isfinite(now) && std::isfinite(state.Started)
        && now >= state.Started && now - state.Started < DurationSeconds) return false;
    if (!owner || slot < 0 || !std::isfinite(now)) return false;
    state = { owner, epoch, slot, now, true };
    return true;
}
inline float NewWeight(const State &state, double now)
{
    if (!state.Active || !std::isfinite(now) || !std::isfinite(state.Started)) return 1;
    const double t = std::clamp((now - state.Started) / DurationSeconds, 0., 1.);
    return static_cast<float>(t * t * (3. - 2. * t));
}
}
