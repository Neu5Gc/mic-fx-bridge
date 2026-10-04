#include "../src/RecoveryState.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>

namespace {

using State = mic_daw::RecoveryState;
using Status = State::Status;
using TimePoint = State::TimePoint;
using Duration = State::Duration;
using namespace std::chrono_literals;

// These are examples from the public backoff contract, not an implementation
// of the state machine used as a second, potentially identical oracle.
constexpr std::array contractualDelays{250ms, 500ms, 1000ms, 2000ms, 4000ms, 5000ms};
constexpr auto oneClockTick = TimePoint::duration{1};

struct Context {
    std::string_view scenario;
    std::uint64_t seed = 0;
    std::uint64_t step = 0;
    std::uint64_t assertions = 0;
    std::uint64_t randomizedActions = 0;
    std::uint64_t failedOpenCycles = 0;
    std::uint64_t flapCycles = 0;
    std::uint64_t toggleCycles = 0;
    std::uint64_t isolatedRetries = 0;
    std::uint64_t healthyOutputHeartbeats = 0;

    void require(bool condition, std::string_view message) {
        ++assertions;
        if (!condition) {
            std::cerr << "FAIL scenario=" << scenario << " seed=" << seed
                      << " step=" << step << ": " << message << '\n';
            throw std::runtime_error("RecoveryState stress assertion failed");
        }
    }
};

// Observe current public state and future watchdog behavior. This lets rejected
// operations and operations on a different endpoint be tested as no-ops.
struct Observation {
    Status status;
    bool automatic;
    Duration timeout;
    std::optional<TimePoint> retry;
    std::array<bool, 5> timedOut;

    bool operator==(const Observation&) const = default;
};

Observation observe(const State& state, TimePoint now) {
    return {state.status(), state.autoRecoveryEnabled(), state.heartbeatTimeout(),
            state.nextRetryAt(),
            {state.heartbeatTimedOut(now - 1ms), state.heartbeatTimedOut(now),
             state.heartbeatTimedOut(now + 1ms), state.heartbeatTimedOut(now + 2s),
             state.heartbeatTimedOut(now + 24h)}};
}

TimePoint deadline(Context& context, const State& state) {
    context.require(state.nextRetryAt().has_value(), "a waiting state has a deadline");
    return *state.nextRetryAt();
}

void invariant(Context& context, const State& state, TimePoint now) {
    const auto waiting = state.status() == Status::waiting;
    context.require(waiting == state.nextRetryAt().has_value(),
                    "only waiting states own retry deadlines");
    context.require(!waiting || state.autoRecoveryEnabled(),
                    "a disabled endpoint cannot retain a scheduled retry");
    context.require(state.heartbeatTimeout() >= 1ms, "watchdog timeout stays positive");
    if (state.status() != Status::running) {
        context.require(!state.heartbeatTimedOut(now + 24h),
                        "inactive states cannot have an expired running watchdog");
    }
}

void scheduledDelayInContract(Context& context, const State& state, TimePoint failureAt) {
    const auto actual = deadline(context, state) - failureAt;
    context.require(std::find(contractualDelays.begin(), contractualDelays.end(), actual)
                        != contractualDelays.end(),
                    "every retry delay belongs to the documented bounded sequence");
}

void dispatch(Context& context, State& state, TimePoint& now, Duration lateness = 0ms) {
    const auto due = deadline(context, state);
    const auto before = observe(state, due - oneClockTick);
    context.require(!state.poll(due - oneClockTick), "one tick early cannot start a retry");
    context.require(observe(state, due - oneClockTick) == before,
                    "an early poll leaves the scheduled attempt untouched");
    now = due + lateness;
    context.require(state.poll(now), "the due or late poll starts exactly one attempt");
    context.require(state.status() == Status::recovering, "a dispatched attempt is recovering");
    context.require(!state.nextRetryAt(), "dispatch consumes the deadline");
    context.require(!state.poll(now), "a repeated due poll cannot duplicate the attempt");
    context.require(!state.requestImmediateRetry(), "manual retry cannot duplicate that attempt");
}

void reachCappedAttempt(Context& context, State& state, TimePoint& now) {
    state.stop();
    state.setAutoRecoveryEnabled(true);
    context.require(state.requestImmediateRetry(), "start the explicit capped-backoff history");
    for (const auto delay : contractualDelays) {
        context.require(state.recoveryFailed(now), "the explicit open attempt can fail");
        context.require(deadline(context, state) == now + delay, "explicit backoff history matches");
        dispatch(context, state, now);
    }
}

void testLongFailureHistory(Context& context) {
    context.scenario = "long-failure-history";
    context.seed = 0xF411EDU;
    std::mt19937_64 random{context.seed};
    State state;
    auto now = TimePoint{};
    context.require(state.requestImmediateRetry(), "initial open is requested");

    for (context.step = 0; context.step < 20000; ++context.step) {
        // Failure completion itself can be delayed; the next deadline must be
        // relative to completion, not to the previous attempt's old deadline.
        now += Duration{static_cast<Duration::rep>(random() % 7001)};
        const auto expected = contractualDelays[std::min<std::size_t>(
            static_cast<std::size_t>(context.step), contractualDelays.size() - 1)];
        context.require(state.recoveryFailed(now), "each in-flight open failure is accepted");
        context.require(deadline(context, state) == now + expected,
                        "failure backoff caps at five seconds without wraparound");
        invariant(context, state, now);
        dispatch(context, state, now, Duration{static_cast<Duration::rep>(random() % 3001)});
        ++context.failedOpenCycles;
    }
}

void testLongFlappingHistory(Context& context) {
    context.scenario = "long-flapping-history";
    context.seed = 0xF1A995EEDULL;
    std::mt19937_64 random{context.seed};
    State state{200ms};
    auto now = TimePoint{};
    state.markRunning(now);

    for (context.step = 0; context.step < 100000; ++context.step) {
        const auto expected = contractualDelays[std::min<std::size_t>(
            static_cast<std::size_t>(context.step), contractualDelays.size() - 1)];
        now += Duration{1 + static_cast<Duration::rep>(random() % 999)};
        state.recordHeartbeat(now);
        context.require(!state.confirmStable(now), "short-lived opens do not reset the backoff");

        if ((context.step & 1U) == 0) {
            context.require(state.deviceLost(now), "explicit device removal is accepted");
        } else {
            now += state.heartbeatTimeout();
            context.require(state.heartbeatTimedOut(now), "the missing callback reaches its timeout");
            context.require(!state.poll(now), "watchdog loss schedules, rather than spins an open");
        }

        context.require(deadline(context, state) == now + expected,
                        "both removal and callback stalls retain the capped flap backoff");
        const auto waiting = observe(state, now);
        context.require(!state.deviceLost(now), "duplicate loss is rejected while waiting");
        context.require(!state.recoverySucceeded(now), "late success is rejected without an attempt");
        context.require(!state.recoveryFailed(now), "late failure is rejected without an attempt");
        context.require(observe(state, now) == waiting, "duplicate callbacks preserve the deadline");
        dispatch(context, state, now);
        context.require(state.recoverySucceeded(now), "the flapping endpoint opens again");
        context.require(!state.recoverySucceeded(now), "completion can only be consumed once");
        invariant(context, state, now);
        ++context.flapCycles;
    }
}

void testHealthyStableReset(Context& context) {
    context.scenario = "healthy-stable-reset";
    context.seed = 0;
    State state;
    auto now = TimePoint{};

    for (context.step = 0; context.step < 256; ++context.step) {
        reachCappedAttempt(context, state, now);
        context.require(state.recoverySucceeded(now), "the capped attempt opens successfully");
        const auto started = now;
        for (auto elapsed = 250ms; elapsed < 10000ms; elapsed += 250ms) {
            now = started + elapsed;
            state.recordHeartbeat(now);
            state.recordHeartbeat(now - 500ms); // deliberately out of order
            context.require(!state.heartbeatTimedOut(now), "regular callbacks keep the watchdog healthy");
            context.require(!state.poll(now), "healthy polling does not request recovery");
            context.require(!state.confirmStable(now), "stability is not confirmed early");
        }

        context.require(!state.confirmStable(started + 10000ms - oneClockTick),
                        "the stable window excludes the preceding clock tick");
        now = started + 10000ms;
        state.recordHeartbeat(now);
        context.require(state.confirmStable(now), "the complete stable window resets backoff");
        context.require(!state.confirmStable(now), "one running period confirms stability only once");
        context.require(!state.heartbeatTimedOut(now + 2000ms - oneClockTick),
                        "old heartbeats cannot move the watchdog threshold earlier");
        context.require(state.heartbeatTimedOut(now + 2000ms), "the newest heartbeat owns the threshold");
        context.require(state.deviceLost(now), "a previously stable endpoint can be lost");
        context.require(deadline(context, state) == now + 250ms,
                        "the first post-stability retry uses the initial delay");
        dispatch(context, state, now);
        context.require(state.recoveryFailed(now), "the first retry may still fail");
        context.require(deadline(context, state) == now + 500ms,
                        "backoff increases again after the stable reset");
    }

    for (const auto timeout : std::array{-5000ms, 0ms, 1ms, 17ms, 2000ms}) {
        State edge{timeout};
        edge.markRunning(now);
        const auto expected = std::max(1ms, timeout);
        context.require(edge.heartbeatTimeout() == expected, "invalid timeouts clamp to one millisecond");
        context.require(!edge.heartbeatTimedOut(now - oneClockTick), "pre-heartbeat times are not expired");
        context.require(!edge.heartbeatTimedOut(now + expected - oneClockTick), "watchdog boundary is exclusive before threshold");
        context.require(edge.heartbeatTimedOut(now + expected), "watchdog boundary is inclusive at threshold");
        context.require(!edge.confirmStable(now, -1ms), "nonpositive stable windows still require time");
        context.require(edge.confirmStable(now + 1ms, 0ms), "nonpositive stable windows clamp to one millisecond");
    }
}

void testToggleCancellationAndStaleCompletions(Context& context) {
    context.scenario = "toggle-cancellation-and-stale-completions";
    context.seed = 0;
    State state;
    auto now = TimePoint{};

    for (context.step = 0; context.step < 10000; ++context.step) {
        state.setAutoRecoveryEnabled(true);
        context.require(state.requestImmediateRetry(), "enabled manual open starts");
        context.require(state.recoveryFailed(now), "failed open schedules backoff");
        const auto cancelledDue = deadline(context, state);
        state.setAutoRecoveryEnabled(false);
        context.require(state.status() == Status::stopped, "disabling cancels a waiting retry");
        context.require(!state.nextRetryAt(), "cancellation removes its deadline");
        state.setAutoRecoveryEnabled(true);
        now = cancelledDue + 1h;
        const auto stopped = observe(state, now);
        context.require(!state.poll(now), "re-enabling alone cannot resurrect a cancelled deadline");
        context.require(!state.recoverySucceeded(now), "cancelled success completion is rejected");
        context.require(!state.recoveryFailed(now), "cancelled failure completion is rejected");
        state.recordHeartbeat(now);
        context.require(observe(state, now) == stopped, "cancelled callbacks cannot resurrect the endpoint");

        context.require(state.requestImmediateRetry(), "a fresh explicit open remains possible");
        state.setAutoRecoveryEnabled(false);
        context.require(state.status() == Status::recovering, "disabling does not cancel an in-flight attempt");
        context.require(!state.requestImmediateRetry(), "disabled in-flight attempts still cannot duplicate");
        context.require(state.recoverySucceeded(now), "the disabled in-flight attempt may succeed");
        state.recordHeartbeat(now + 1ms);
        context.require(state.status() == Status::running, "disabling preserves an already-open stream");
        context.require(state.deviceLost(now + 1ms), "disabled running loss is consumed");
        context.require(state.status() == Status::stopped, "disabled loss stops without retrying");

        context.require(state.requestImmediateRetry(), "manual retry works while disabled");
        context.require(state.recoveryFailed(now + 2ms), "the disabled manual attempt may fail");
        context.require(state.status() == Status::stopped, "disabled failures do not schedule work");
        context.require(state.requestImmediateRetry(), "another explicit attempt can start");
        state.stop();
        context.require(!state.recoverySucceeded(now + 3ms), "stop rejects an old successful completion");
        context.require(!state.recoveryFailed(now + 3ms), "stop rejects an old failed completion");
        context.require(!state.autoRecoveryEnabled(), "stop preserves the user's automatic recovery preference");
        invariant(context, state, now);
        now += 4ms;
        ++context.toggleCycles;
    }
}

void testIndependentEndpoints(Context& context) {
    context.scenario = "independent-endpoints";
    context.seed = 0;
    State input;
    State output;
    auto now = TimePoint{};
    output.markRunning(now);
    context.require(input.requestImmediateRetry(), "input recovery starts independently");

    for (context.step = 0; context.step < 10000; ++context.step) {
        const auto outputBeforeFailure = observe(output, now);
        context.require(input.recoveryFailed(now), "the flaky input open fails");
        context.require(observe(output, now) == outputBeforeFailure,
                        "input failure does not mutate the running output");
        const auto due = deadline(context, input);
        while (now < due) {
            now += std::min<TimePoint::duration>(250ms, due - now);
            const auto inputBeforeHeartbeat = observe(input, now);
            output.recordHeartbeat(now);
            context.require(!output.poll(now), "the healthy virtual output never requests recovery");
            static_cast<void>(output.confirmStable(now));
            context.require(output.status() == Status::running, "input downtime leaves output running");
            context.require(observe(input, now) == inputBeforeHeartbeat,
                            "output heartbeats and stable reset cannot alter the input retry");
            ++context.healthyOutputHeartbeats;
        }

        const auto outputBeforeRetry = observe(output, now);
        context.require(input.poll(now), "the isolated input deadline starts its retry");
        context.require(observe(output, now) == outputBeforeRetry, "input retry does not stop the virtual output");

        if (context.step >= 5 && context.step % 127 == 0) {
            // Branch each history by value so these probes do not disturb the
            // long-running pair. A capped input and a healthy output must have
            // independent backoff histories, not shared/static retry state.
            auto inputProbe = input;
            auto outputProbe = output;
            context.require(outputProbe.deviceLost(now), "the independent output probe can lose its device");
            context.require(deadline(context, outputProbe) == now + 250ms,
                            "many input failures never inflate output backoff");
            context.require(inputProbe.recoveryFailed(now), "the capped input probe can fail again");
            context.require(deadline(context, inputProbe) == now + 5000ms,
                            "healthy output stability never resets the input backoff");
            outputProbe.setAutoRecoveryEnabled(false);
            context.require(inputProbe.autoRecoveryEnabled(), "output cancellation cannot disable input recovery");
            context.require(inputProbe.nextRetryAt().has_value(), "output cancellation cannot erase input deadlines");
        }
        ++context.isolatedRetries;
    }
}

void testRandomizedContract(Context& context, std::uint64_t seed) {
    context.scenario = "randomized-public-contract";
    context.seed = seed;
    std::mt19937_64 random{seed};
    State state{Duration{1 + static_cast<Duration::rep>(random() % 2000)}};
    auto now = TimePoint{};
    std::array<std::uint64_t, 10> actionCounts{};

    for (context.step = 0; context.step < 250000; ++context.step) {
        now += Duration{static_cast<Duration::rep>(random() % 1500)};
        const auto before = observe(state, now);
        const auto action = static_cast<std::size_t>(random() % actionCounts.size());
        ++actionCounts[action];
        ++context.randomizedActions;

        switch (action) {
            case 0:
                state.stop();
                context.require(state.status() == Status::stopped, "stop always reaches stopped");
                context.require(state.autoRecoveryEnabled() == before.automatic, "stop preserves the auto preference");
                break;
            case 1: {
                const bool enabled = (random() & 1U) != 0;
                state.setAutoRecoveryEnabled(enabled);
                context.require(state.autoRecoveryEnabled() == enabled, "automatic preference is stored");
                context.require(state.status() == ((!enabled && before.status == Status::waiting)
                                                      ? Status::stopped : before.status),
                                "only pending retries are cancelled by disabling");
                break;
            }
            case 2: {
                const bool accepted = state.requestImmediateRetry();
                context.require(accepted == (before.status != Status::recovering), "only duplicate in-flight requests are rejected");
                context.require(state.status() == Status::recovering, "manual request leaves exactly one in-flight attempt");
                if (!accepted)
                    context.require(observe(state, now) == before, "duplicate manual request is a no-op");
                break;
            }
            case 3: {
                const bool accepted = state.recoverySucceeded(now);
                context.require(accepted == (before.status == Status::recovering), "success requires an in-flight attempt");
                if (accepted) {
                    context.require(state.status() == Status::running, "accepted success starts running");
                    context.require(!state.heartbeatTimedOut(now), "success establishes a fresh watchdog");
                } else {
                    context.require(observe(state, now) == before, "rejected success cannot modify the endpoint");
                }
                break;
            }
            case 4: {
                const bool accepted = state.recoveryFailed(now);
                context.require(accepted == (before.status == Status::recovering), "failure requires an in-flight attempt");
                if (!accepted)
                    context.require(observe(state, now) == before, "rejected failure cannot modify the endpoint");
                else if (before.automatic)
                    scheduledDelayInContract(context, state, now);
                else
                    context.require(state.status() == Status::stopped, "disabled failure stops");
                break;
            }
            case 5: {
                const bool accepted = state.deviceLost(now);
                context.require(accepted == (before.status == Status::running), "loss requires a running endpoint");
                if (!accepted)
                    context.require(observe(state, now) == before, "rejected loss cannot modify the endpoint");
                else if (before.automatic)
                    scheduledDelayInContract(context, state, now);
                else
                    context.require(state.status() == Status::stopped, "disabled loss stops");
                break;
            }
            case 6: {
                const auto heartbeat = now - Duration{static_cast<Duration::rep>(random() % 5000)};
                state.recordHeartbeat(heartbeat);
                if (before.status != Status::running) {
                    context.require(observe(state, now) == before, "heartbeats outside running are ignored");
                } else {
                    context.require(state.status() == Status::running, "heartbeat alone cannot change running status");
                    if (!before.timedOut[1])
                        context.require(!state.heartbeatTimedOut(now), "old activity cannot make a healthy watchdog expire");
                }
                break;
            }
            case 7: {
                const bool expectedDispatch = before.status == Status::waiting && before.retry && now >= *before.retry;
                const bool accepted = state.poll(now);
                context.require(accepted == expectedDispatch, "poll dispatches exactly a due pending retry");
                if (expectedDispatch) {
                    context.require(state.status() == Status::recovering, "a due poll enters recovering");
                } else if (before.status == Status::running && before.timedOut[1]) {
                    if (before.automatic)
                        scheduledDelayInContract(context, state, now);
                    else
                        context.require(state.status() == Status::stopped, "disabled watchdog timeout stops");
                } else {
                    context.require(observe(state, now) == before, "non-due non-expired polling is a no-op");
                }
                break;
            }
            case 8: {
                const auto window = Duration{static_cast<Duration::rep>(random() % 12000) - 1000};
                const bool confirmed = state.confirmStable(now, window);
                context.require(observe(state, now) == before, "stability confirmation does not replace endpoint status or heartbeats");
                if (confirmed) {
                    context.require(before.status == Status::running, "only a running endpoint can be stable");
                    context.require(!state.confirmStable(now, window), "stable confirmation is idempotent");
                    if (state.autoRecoveryEnabled()) {
                        auto probe = state;
                        context.require(probe.deviceLost(now), "a confirmed running history supports a loss probe");
                        context.require(deadline(context, probe) == now + 250ms, "every accepted stable reset returns to initial backoff");
                    }
                }
                break;
            }
            case 9:
                state.markRunning(now);
                context.require(state.status() == Status::running, "owner-reported open starts running");
                context.require(!state.heartbeatTimedOut(now + state.heartbeatTimeout() - oneClockTick),
                                "owner-reported open establishes a fresh heartbeat");
                context.require(state.heartbeatTimedOut(now + state.heartbeatTimeout()),
                                "owner-reported open has the documented timeout boundary");
                break;
        }
        invariant(context, state, now);
    }

    for (const auto count : actionCounts)
        context.require(count > 20000, "the reproducible walk exercises every action substantially");
    std::cout << "Seed " << seed << ": 250000 randomized actions passed; action counts";
    for (const auto count : actionCounts)
        std::cout << ' ' << count;
    std::cout << '\n';
}

} // namespace

int main() {
    Context context;
    try {
        testLongFailureHistory(context);
        testLongFlappingHistory(context);
        testHealthyStableReset(context);
        testToggleCancellationAndStaleCompletions(context);
        testIndependentEndpoints(context);
        for (const auto seed : std::array<std::uint64_t, 4>{0x5EEDU, 0xC0FFEEU, 0xDEADBEEFU, 0x123456789ABCDEF0ULL})
            testRandomizedContract(context, seed);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "All RecoveryState stress tests passed\n"
              << "Assertions: " << context.assertions << '\n'
              << "Randomized actions: " << context.randomizedActions << '\n'
              << "Failed-open cycles: " << context.failedOpenCycles << '\n'
              << "Short-lived success/loss cycles: " << context.flapCycles << '\n'
              << "Recovery toggle/cancellation histories: " << context.toggleCycles << '\n'
              << "Independent input retries: " << context.isolatedRetries << '\n'
              << "Healthy output heartbeats during input downtime: " << context.healthyOutputHeartbeats << '\n'
              << "All clocks are synthetic; no audio devices or user settings are accessed.\n";
    return EXIT_SUCCESS;
}
