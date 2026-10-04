#include "RecoveryState.h"

#include <algorithm>

namespace mic_daw {

RecoveryState::RecoveryState(Duration heartbeatTimeout) noexcept
    : heartbeatTimeout_(std::max(heartbeatTimeout, Duration{1})) {}

RecoveryState::Status RecoveryState::status() const noexcept {
    return status_;
}

bool RecoveryState::autoRecoveryEnabled() const noexcept {
    return autoRecoveryEnabled_;
}

RecoveryState::Duration RecoveryState::heartbeatTimeout() const noexcept {
    return heartbeatTimeout_;
}

std::optional<RecoveryState::TimePoint> RecoveryState::nextRetryAt() const noexcept {
    return nextRetryAt_;
}

void RecoveryState::setAutoRecoveryEnabled(bool enabled) noexcept {
    autoRecoveryEnabled_ = enabled;

    if (!enabled && status_ == Status::waiting) {
        stop();
    }
}

void RecoveryState::stop() noexcept {
    status_ = Status::stopped;
    lastHeartbeat_.reset();
    nextRetryAt_.reset();
    runningSince_.reset();
    stableConfirmed_ = false;
    resetBackoff();
}

void RecoveryState::markRunning(TimePoint now) noexcept {
    status_ = Status::running;
    lastHeartbeat_ = now;
    nextRetryAt_.reset();
    runningSince_ = now;
    stableConfirmed_ = false;
}

bool RecoveryState::confirmStable(TimePoint now, Duration stableWindow) noexcept {
    stableWindow = std::max(stableWindow, Duration{1});

    if (status_ != Status::running || !runningSince_ || stableConfirmed_
        || now < *runningSince_ || now - *runningSince_ < stableWindow) {
        return false;
    }

    resetBackoff();
    stableConfirmed_ = true;
    return true;
}

void RecoveryState::recordHeartbeat(TimePoint now) noexcept {
    if (status_ != Status::running) {
        return;
    }

    if (!lastHeartbeat_ || now >= *lastHeartbeat_) {
        lastHeartbeat_ = now;
    }
}

bool RecoveryState::heartbeatTimedOut(TimePoint now) const noexcept {
    if (status_ != Status::running || !lastHeartbeat_ || now < *lastHeartbeat_) {
        return false;
    }

    return now - *lastHeartbeat_ >= heartbeatTimeout_;
}

bool RecoveryState::requestImmediateRetry() noexcept {
    if (status_ == Status::recovering) {
        return false;
    }

    status_ = Status::recovering;
    lastHeartbeat_.reset();
    nextRetryAt_.reset();
    runningSince_.reset();
    stableConfirmed_ = false;
    return true;
}

bool RecoveryState::deviceLost(TimePoint now) noexcept {
    if (status_ != Status::running) {
        return false;
    }

    lastHeartbeat_.reset();
    runningSince_.reset();
    stableConfirmed_ = false;

    if (!autoRecoveryEnabled_) {
        stop();
        return true;
    }

    status_ = Status::waiting;
    nextRetryAt_ = now + nextBackoff_;
    increaseBackoff();
    return true;
}

bool RecoveryState::poll(TimePoint now) noexcept {
    if (status_ == Status::waiting) {
        if (!autoRecoveryEnabled_) {
            stop();
            return false;
        }

        if (nextRetryAt_ && now >= *nextRetryAt_) {
            status_ = Status::recovering;
            nextRetryAt_.reset();
            return true;
        }
        return false;
    }

    if (status_ == Status::running && heartbeatTimedOut(now))
        static_cast<void>(deviceLost(now));

    return false;
}

bool RecoveryState::recoverySucceeded(TimePoint now) noexcept {
    if (status_ != Status::recovering) {
        return false;
    }

    markRunning(now);
    return true;
}

bool RecoveryState::recoveryFailed(TimePoint now) noexcept {
    if (status_ != Status::recovering) {
        return false;
    }

    lastHeartbeat_.reset();
    runningSince_.reset();
    stableConfirmed_ = false;

    if (!autoRecoveryEnabled_) {
        stop();
        return true;
    }

    status_ = Status::waiting;
    nextRetryAt_ = now + nextBackoff_;
    increaseBackoff();
    return true;
}

void RecoveryState::resetBackoff() noexcept {
    nextBackoff_ = kInitialBackoff;
}

void RecoveryState::increaseBackoff() noexcept {
    // Add at most the remaining distance to the cap, avoiding duration overflow.
    const auto remaining = kMaximumBackoff - nextBackoff_;
    nextBackoff_ += std::min(nextBackoff_, remaining);
}

} // namespace mic_daw
