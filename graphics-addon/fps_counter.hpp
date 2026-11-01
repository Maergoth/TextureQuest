#pragma once
#include <cstdint>
#include <mutex>
#include "texturequest_aio_api.h"

namespace tq {
inline bool AioOutputActive(bool compatible, bool original, uint32_t flags,
                            const TextureQuestAIOControlStatusV2* controls) {
    constexpr uint32_t required = TQAIO_UserEnabled | TQAIO_MasterAdmitted;
    return compatible && !original && (flags & required) == required &&
        (!controls || controls->Applied.EnabledMask != 0);
}

struct FrameRates {
    bool Ready = false, OutputActive = false;
    double Game = 0, Output = 0;
};

// Source callbacks count rendered game frames. Output counts are accepted
// compositor Presents, including generated frames; they do not prove scanout.
class FpsCounter {
    static constexpr uint64_t SampleIntervalMs = 500;
    std::mutex Lock;
    bool Started = false, OutputActive = false;
    uint64_t Start = 0, Previous = 0, GameFrames = 0, OutputStart = 0, OutputPrevious = 0;
    uint64_t OutputAdvanced = 0;
    FrameRates Last{};

    void Restart(uint64_t now, bool active, uint64_t output) {
        Started = true; OutputActive = active;
        Start = Previous = now; GameFrames = 0;
        OutputStart = OutputPrevious = output;
        OutputAdvanced = now;
        Last = {}; Last.OutputActive = active;
    }
public:
    void Frame(uint64_t now, bool active, uint64_t output) {
        std::lock_guard guard(Lock);
        if (!Started || now < Previous || now - Previous > 3000 || active != OutputActive ||
            (active && output < OutputPrevious)) {
            Restart(now, active, output);
            return;
        }
        if (active && output != OutputPrevious) OutputAdvanced = now;
        const bool stalled = active && now - OutputAdvanced >= SampleIntervalMs;
        // An old burst must not leave a positive Output reading until the next
        // window boundary. Continued game callbacks age it independently.
        if (stalled) Last.Output = 0;
        ++GameFrames;
        Previous = now; OutputPrevious = output;
        const uint64_t elapsed = now - Start;
        if (elapsed < SampleIntervalMs) return;
        Last.Ready = true; Last.OutputActive = active;
        Last.Game = 1000.0 * double(GameFrames) / double(elapsed);
        Last.Output = active ? (stalled ? 0 : 1000.0 * double(output - OutputStart) / double(elapsed)) : Last.Game;
        Start = now; GameFrames = 0; OutputStart = output;
    }
    FrameRates Snapshot() {
        std::lock_guard guard(Lock);
        return Last;
    }
};
} // namespace tq
