#include "../src/RecoveryState.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using mic_daw::RecoveryState;
using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

RecoveryState::TimePoint at(std::chrono::milliseconds offset) {
    return RecoveryState::TimePoint{} + offset;
}

void testImmediateRetryAndSuccess() {
    RecoveryState state;

    expect(state.status() == RecoveryState::Status::stopped, "starts stopped");
    expect(state.requestImmediateRetry(), "immediate retry starts recovery");
    expect(state.status() == RecoveryState::Status::recovering, "retry is recovering");
    expect(!state.requestImmediateRetry(), "does not duplicate an active recovery");
    expect(state.recoverySucceeded(at(10ms)), "active recovery can succeed");
    expect(state.status() == RecoveryState::Status::running, "success is running");
    expect(!state.nextRetryAt().has_value(), "success clears retry deadline");
}

void testExponentialBackoffAndCap() {
    RecoveryState state;
    auto now = at(0ms);
    constexpr std::array delays{250ms, 500ms, 1000ms, 2000ms, 4000ms, 5000ms, 5000ms};

    expect(state.requestImmediateRetry(), "initial attempt starts");

    for (const auto delay : delays) {
        expect(state.recoveryFailed(now), "active recovery can fail");
        expect(state.status() == RecoveryState::Status::waiting, "failure waits");
        expect(state.nextRetryAt() == now + delay, "retry deadline uses expected backoff");
        expect(!state.poll(now + delay - 1ms), "does not retry before deadline");
        now += delay;
        expect(state.poll(now), "retries at deadline");
        expect(state.status() == RecoveryState::Status::recovering, "deadline starts recovery");
    }

    expect(state.recoverySucceeded(now), "eventual recovery succeeds");
    expect(!state.confirmStable(now + 9999ms), "running device is not stable too early");
    const auto stableAt = now + 10000ms;
    expect(state.confirmStable(stableAt), "stable window resets backoff");
    expect(state.requestImmediateRetry(), "manual retry can restart a running device");
    expect(state.recoveryFailed(stableAt), "post-success attempt can fail");
    expect(state.nextRetryAt() == stableAt + 250ms, "stable run resets backoff to 250 ms");
}

void testHeartbeatTimeout() {
    RecoveryState state{1000ms};
    state.markRunning(at(0ms));
    state.recordHeartbeat(at(100ms));

    expect(!state.heartbeatTimedOut(at(1099ms)), "heartbeat is healthy before timeout");
    expect(!state.poll(at(1099ms)), "healthy heartbeat does not recover");
    expect(state.heartbeatTimedOut(at(1100ms)), "heartbeat times out at threshold");
    expect(!state.poll(at(1100ms)), "timeout schedules a guarded retry");
    expect(state.status() == RecoveryState::Status::waiting, "timeout enters backoff");
    expect(state.nextRetryAt() == at(1350ms), "timeout uses initial backoff");
    expect(state.poll(at(1350ms)), "timeout retry starts at its deadline");
    expect(state.status() == RecoveryState::Status::recovering, "deadline is recovering");
}

void testAutomaticRecoveryToggle() {
    RecoveryState state{500ms};
    state.markRunning(at(0ms));
    state.setAutoRecoveryEnabled(false);

    expect(!state.autoRecoveryEnabled(), "automatic recovery can be disabled");
    expect(state.heartbeatTimedOut(at(500ms)), "timeout detection remains available");
    expect(!state.poll(at(500ms)), "disabled timeout does not request recovery");
    expect(state.status() == RecoveryState::Status::stopped, "disabled timeout stops");

    expect(state.requestImmediateRetry(), "manual retry works while automatic recovery is off");
    expect(state.recoveryFailed(at(600ms)), "manual attempt can fail");
    expect(state.status() == RecoveryState::Status::stopped, "disabled failure does not schedule retry");

    state.setAutoRecoveryEnabled(true);
    expect(state.autoRecoveryEnabled(), "automatic recovery can be re-enabled");
    expect(state.requestImmediateRetry(), "retry starts after re-enable");
    expect(state.recoveryFailed(at(700ms)), "automatic attempt can fail");
    expect(state.status() == RecoveryState::Status::waiting, "enabled failure schedules retry");
    state.setAutoRecoveryEnabled(false);
    expect(state.status() == RecoveryState::Status::stopped, "disabling cancels pending retry");
    expect(!state.nextRetryAt().has_value(), "cancelled retry clears deadline");
}

void testFlappingKeepsBackoff() {
    RecoveryState state;
    auto now = at(0ms);

    expect(state.requestImmediateRetry(), "flap initial open starts");
    expect(state.recoveryFailed(now), "first open fails");
    now += 250ms;
    expect(state.poll(now), "first retry starts");
    expect(state.recoverySucceeded(now), "retry opens");
    expect(state.deviceLost(now + 100ms), "unstable running device is lost");
    expect(state.nextRetryAt() == now + 100ms + 500ms,
           "short-lived success preserves increased backoff");
}

void testInvalidTransitionsAndOldHeartbeat() {
    RecoveryState state{1000ms};

    expect(!state.recoverySucceeded(at(0ms)), "success is rejected while stopped");
    expect(!state.recoveryFailed(at(0ms)), "failure is rejected while stopped");
    state.markRunning(at(100ms));
    state.recordHeartbeat(at(50ms));
    expect(!state.heartbeatTimedOut(at(1099ms)), "older heartbeat timestamp is ignored");
    expect(state.heartbeatTimedOut(at(1100ms)), "original heartbeat remains authoritative");
}

} // namespace

int main() {
    testImmediateRetryAndSuccess();
    testExponentialBackoffAndCap();
    testHeartbeatTimeout();
    testAutomaticRecoveryToggle();
    testInvalidTransitionsAndOldHeartbeat();
    testFlappingKeepsBackoff();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All RecoveryState tests passed\n";
    return EXIT_SUCCESS;
}
