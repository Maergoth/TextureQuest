#pragma once
#include <cstdint>
namespace tq_dispatch_overlap {
enum class Plan { Hold, ConsumeThenStage, StageOnly };
constexpr Plan Select(bool fg_held, bool protected_guide_pending,
    bool flow_provider_ready, bool output_capacity) noexcept {
    if (!fg_held) return output_capacity ? Plan::ConsumeThenStage : Plan::Hold;
    return flow_provider_ready && !protected_guide_pending ? Plan::StageOnly : Plan::Hold;
}
// Queue ordering: preparation may already read the source. Pin source/slot to
// the newly reserved real completion value BEFORE publishing the queue signal.
// A failed signal keeps that ownership pinned and requires normal failure;
// it never invents a completed job or returns the source to the capture queue.
template<class Reserve, class Pin, class Signal>
bool RetirePreparation(Reserve reserve, Pin pin, Signal signal) {
    const std::uint64_t value=reserve();
    if (!value) return false;
    pin(value);
    return signal(value);
}
}
