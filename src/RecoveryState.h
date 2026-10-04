#pragma once

#include <chrono>
#include <optional>

namespace mic_daw {

// A small, single-threaded state machine for supervising an audio device.
// The owner is responsible for serializing calls and performing the actual
// device open/close work whenever poll() or requestImmediateRetry() returns
// true.
class RecoveryState final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::milliseconds;

    enum class Status {
        stopped,
        waiting,
        recovering,
        running,
    };

    static constexpr Duration kInitialBackoff{250};
    static constexpr Duration kMaximumBackoff{5000};
    static constexpr Duration kStableWindow{10000};

    explicit RecoveryState(Duration heartbeatTimeout = Duration{2000}) noexcept;

    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] bool autoRecoveryEnabled() const noexcept;
    [[nodiscard]] Duration heartbeatTimeout() const noexcept;
    [[nodiscard]] std::optional<TimePoint> nextRetryAt() const noexcept;

    // Disabling automatic recovery cancels a pending backoff retry. An attempt
    // already in progress is allowed to finish; a subsequent failure stops.
    void setAutoRecoveryEnabled(bool enabled) noexcept;

    // Stops supervision and resets the exponential backoff to 250 ms.
    void stop() noexcept;

    // Marks an already-open device as running and establishes the first
    // heartbeat. Backoff is reset only after confirmStable(), so a device that
    // repeatedly opens and drops cannot create a tight reopen loop.
    void markRunning(TimePoint now) noexcept;
    [[nodiscard]] bool confirmStable(TimePoint now,
                                     Duration stableWindow = kStableWindow) noexcept;

    // Records activity only while running. Older timestamps are ignored.
    void recordHeartbeat(TimePoint now) noexcept;
    [[nodiscard]] bool heartbeatTimedOut(TimePoint now) const noexcept;

    // Starts a recovery attempt immediately, including when automatic recovery
    // is disabled. Returns true only when the caller should start new work.
    [[nodiscard]] bool requestImmediateRetry() noexcept;

    // Schedules a backoff after a running stream is lost.
    [[nodiscard]] bool deviceLost(TimePoint now) noexcept;

    // Advances scheduled work. Returns true exactly when the caller should
    // start a recovery attempt. Heartbeat timeouts and open failures use the
    // same exponential backoff.
    [[nodiscard]] bool poll(TimePoint now) noexcept;

    // Complete the current attempt. Calls made outside recovering are rejected.
    [[nodiscard]] bool recoverySucceeded(TimePoint now) noexcept;
    [[nodiscard]] bool recoveryFailed(TimePoint now) noexcept;

private:
    void resetBackoff() noexcept;
    void increaseBackoff() noexcept;

    Status status_{Status::stopped};
    bool autoRecoveryEnabled_{true};
    Duration heartbeatTimeout_;
    Duration nextBackoff_{kInitialBackoff};
    std::optional<TimePoint> lastHeartbeat_;
    std::optional<TimePoint> nextRetryAt_;
    std::optional<TimePoint> runningSince_;
    bool stableConfirmed_{false};
};

} // namespace mic_daw
