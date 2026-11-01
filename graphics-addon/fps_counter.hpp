#pragma once
#include <cstdint>
#include <mutex>

namespace tq {
struct FrameRates {
    bool Ready = false, OutputActive = false;
    double Game = 0, Output = 0;
};

// Source callbacks count rendered game frames. Output counts are accepted
// compositor Presents, including generated frames; they do not prove scanout.
class FpsCounter {
    std::mutex Lock;
    bool Started = false, OutputActive = false;
    uint64_t Start = 0, Previous = 0, GameFrames = 0, OutputStart = 0, OutputPrevious = 0;
    FrameRates Last{};

    void Restart(uint64_t now, bool active, uint64_t output) {
        Started = true; OutputActive = active;
        Start = Previous = now; GameFrames = 0;
        OutputStart = OutputPrevious = output;
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
        ++GameFrames;
        Previous = now; OutputPrevious = output;
        const uint64_t elapsed = now - Start;
        if (elapsed < 500) return;
        Last.Ready = true; Last.OutputActive = active;
        Last.Game = 1000.0 * double(GameFrames) / double(elapsed);
        Last.Output = active ? 1000.0 * double(output - OutputStart) / double(elapsed) : Last.Game;
        Start = now; GameFrames = 0; OutputStart = output;
    }
    FrameRates Snapshot() {
        std::lock_guard guard(Lock);
        return Last;
    }
};
} // namespace tq
