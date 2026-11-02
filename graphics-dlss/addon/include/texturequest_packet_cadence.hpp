#pragma once
#include <atomic>
#include <cstdint>

// Single completion-observer writer; arbitrary presentation-reader. This is
// observed fence-ready cadence, not a GPU timestamp or a scanout measurement.
// Presentation never supplies samples, so its own waits cannot feed back into
// the real-packet estimator. Epoch changes and long gaps invalidate old data.
namespace tq_packet_cadence {
class Estimator {
    std::uint64_t owner_epoch_ = 0, last_fence_ = 0, last_us_ = 0, period_us_ = 0;
    std::atomic<std::uint64_t> published_epoch_{0}, published_period_us_{0};
    void Publish(std::uint64_t epoch, std::uint64_t period) noexcept {
        published_epoch_.store(0, std::memory_order_release);
        published_period_us_.store(period, std::memory_order_release);
        published_epoch_.store(epoch, std::memory_order_release);
    }
public:
    // Called only by the existing real-neural-fence completion observer.
    void Observe(std::uint64_t epoch, std::uint64_t fence, std::uint64_t now_us) noexcept {
        if (!epoch || !fence || fence == UINT64_MAX || !now_us) return;
        if (epoch != owner_epoch_) {
            owner_epoch_ = epoch; last_fence_ = fence; last_us_ = now_us; period_us_ = 0;
            Publish(epoch, 0); return;
        }
        if (fence <= last_fence_ || now_us <= last_us_) return;
        const auto elapsed = now_us - last_us_;
        last_fence_ = fence; last_us_ = now_us;
        if (elapsed > 250000) { period_us_ = 0; Publish(epoch, 0); return; }
        // Coalesced/duplicate event bursts are not independent 1000+FPS packets.
        if (elapsed < 1000) return;
        const auto bounded = elapsed > 80000 ? 80000 : elapsed;
        period_us_ = period_us_ ? (period_us_ * 3 + bounded + 2) / 4 : bounded;
        Publish(epoch, period_us_);
    }
    std::uint64_t PeriodUs(std::uint64_t epoch) const noexcept {
        if (!epoch || published_epoch_.load(std::memory_order_acquire) != epoch) return 0;
        const auto value = published_period_us_.load(std::memory_order_acquire);
        return published_epoch_.load(std::memory_order_acquire) == epoch ? value : 0;
    }
    std::int64_t OutputIntervalQpc(std::uint64_t epoch, std::int64_t frequency,
        std::int64_t refresh_floor, unsigned outputs) const noexcept {
        if (frequency <= 0 || refresh_floor <= 0 || outputs < 2 || outputs > 4)
            return refresh_floor;
        const auto period = PeriodUs(epoch);
        if (!period) return refresh_floor;
        const auto interval_us = (period + outputs / 2) / outputs;
        const auto interval = (static_cast<std::uint64_t>(frequency) * interval_us + 500000) / 1000000;
        return interval > static_cast<std::uint64_t>(refresh_floor) ?
            static_cast<std::int64_t>(interval) : refresh_floor;
    }
};
}
