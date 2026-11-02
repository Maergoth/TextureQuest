#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

// Slot states own the non-atomic metadata. Retiring is a short CPU claim,
// never a GPU-completion substitute. Only Free admits a new producer.
enum LegacyCaptureSlotState : unsigned int
{
    LegacyCaptureSlotFree = 0,
    LegacyCaptureSlotRecording = 1,
    LegacyCaptureSlotQueued = 2,
    LegacyCaptureSlotReserved = 3,
    LegacyCaptureSlotNeuralReading = 4,
    LegacyCaptureSlotNvofReading = 5,
    LegacyCaptureSlotRetiring = 6,
    LegacyCaptureSlotAbandoned = 7, // Ambiguous failed submission; normal drain only.
};

namespace texturequest_capture_ring
{
template<class Slot>
bool Claim(Slot &slot, unsigned int from, unsigned int to)
{
    return slot.state.compare_exchange_strong(from, to,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

template<class Slot, std::size_t Count, class Index>
int AcquireFree(Slot (&slots)[Count], Index &next)
{
    static_assert(Count > 0);
    for (std::size_t offset = 0; offset < Count; ++offset)
    {
        const auto index = (static_cast<std::size_t>(next) + offset) % Count;
        if (!Claim(slots[index], LegacyCaptureSlotFree, LegacyCaptureSlotRecording))
            continue;
        next = static_cast<Index>((index + 1) % Count);
        return static_cast<int>(index);
    }
    return -1;
}

// A single immediate-context source producer may queue one capture ahead of
// already admitted consumers. State snapshots only bound work admission;
// AcquireFree's exclusive CAS remains the authority for resource custody.
template<class Slot, std::size_t Count, class Index>
int AcquireLatest(Slot (&slots)[Count], Index &next, bool &pending)
{
    pending = false;
    for (auto &slot : slots)
    {
        const unsigned int state = slot.state.load(std::memory_order_acquire);
        if (state == LegacyCaptureSlotRecording || state == LegacyCaptureSlotQueued ||
            state == LegacyCaptureSlotRetiring)
        {
            pending = true;
            return -1;
        }
    }
    return AcquireFree(slots, next);
}

// Caller owns Retiring. Clear every per-use field BEFORE publishing Free;
// a producer may acquire Free immediately, even before this function returns.
template<class Slot>
void PublishFree(Slot &slot)
{
    slot.capture_fence_value = 0;
    slot.neural_fence_value = 0;
    slot.nvof_fence_value = 0;
    slot.sequence = 0;
    slot.submitted_qpc.QuadPart = 0;
    slot.completed_qpc.store(0, std::memory_order_relaxed);
    slot.state.store(LegacyCaptureSlotFree, std::memory_order_release);
}

template<class Slot>
bool ReclaimNeural(Slot &slot, std::uint64_t completed)
{
    if (!Claim(slot, LegacyCaptureSlotNeuralReading, LegacyCaptureSlotRetiring))
        return false;
    if (slot.neural_fence_value != 0 && completed >= slot.neural_fence_value)
    {
        PublishFree(slot);
        return true;
    }
    slot.state.store(LegacyCaptureSlotNeuralReading, std::memory_order_release);
    return false;
}

// Inspect metadata only after claiming Queued, so a concurrent retirement /
// fresh Recording use cannot race the fence or sequence reads.
template<class Slot>
bool DiscardReady(Slot &slot, std::uint64_t completed,
    unsigned long long before_sequence, bool every_sequence = false)
{
    if (!Claim(slot, LegacyCaptureSlotQueued, LegacyCaptureSlotRetiring))
        return false;
    if (slot.capture_fence_value != 0 && completed >= slot.capture_fence_value &&
        (every_sequence || slot.sequence < before_sequence))
    {
        PublishFree(slot);
        return true;
    }
    slot.state.store(LegacyCaptureSlotQueued, std::memory_order_release);
    return false;
}

template<class Slot, std::size_t Count>
int ReserveNewestReady(Slot (&slots)[Count], std::uint64_t completed,
    unsigned int &discarded)
{
    int selected = -1;
    unsigned long long newest = 0;
    for (std::size_t index = 0; index < Count; ++index)
    {
        auto &slot = slots[index];
        if (!Claim(slot, LegacyCaptureSlotQueued, LegacyCaptureSlotReserved))
            continue;
        if (slot.capture_fence_value == 0 || completed < slot.capture_fence_value ||
            (selected >= 0 && slot.sequence < newest))
        {
            slot.state.store(LegacyCaptureSlotQueued, std::memory_order_release);
            continue;
        }
        if (selected >= 0)
            slots[selected].state.store(LegacyCaptureSlotQueued, std::memory_order_release);
        selected = static_cast<int>(index);
        newest = slot.sequence;
    }
    if (selected >= 0)
        for (std::size_t index = 0; index < Count; ++index)
            if (index != static_cast<std::size_t>(selected) &&
                DiscardReady(slots[index], completed, newest))
                ++discarded;
    return selected;
}
} // namespace texturequest_capture_ring
