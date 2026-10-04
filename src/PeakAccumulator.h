#pragma once

#include <atomic>
#include <cmath>

namespace mic_daw
{
// Meter metadata only: retain short audio-block peaks between slower UI ticks.
// publish() neither modifies audio nor applies gain. One UI consumer calls
// take(), which consumes the maximum since the previous take/reset. Concurrent
// publication is assigned to one of those adjacent windows, never overwritten
// by a later, quieter block. There is no timed hold/decay or ADC calibration here.
class PeakAccumulator final
{
public:
    // A peak is a nonnegative magnitude. Ignore malformed metadata, but keep
    // finite values above 1.0 so the consumer can display over-full-scale peaks.
    void publish(float blockPeak) noexcept
    {
        if (!std::isfinite(blockPeak) || blockPeak <= 0.0f)
            return;

        auto current = pending.load(std::memory_order_relaxed);
        while (blockPeak > current
               && !pending.compare_exchange_weak(current, blockPeak,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed))
        {
            // A UI exchange may have cleared the previous maximum. Retry
            // against the updated value instead of losing the incoming peak.
        }
    }

    // Deliberately consuming despite const: an engine's const snapshot API can
    // drain meter history. Multiple consumers would split the peaks between them.
    [[nodiscard]] float take() const noexcept
    {
        return pending.exchange(0.0f, std::memory_order_relaxed);
    }

    // Intentional history discard for stream stop/restart or a route change.
    // Ordinary silent blocks must publish(0), not reset(), to retain earlier
    // peaks in the current UI window.
    void reset() noexcept
    {
        pending.store(0.0f, std::memory_order_relaxed);
    }

private:
    static_assert(std::atomic<float>::is_always_lock_free,
                  "PeakAccumulator requires lock-free float atomics");
    mutable std::atomic<float> pending { 0.0f };
};
} // namespace mic_daw
