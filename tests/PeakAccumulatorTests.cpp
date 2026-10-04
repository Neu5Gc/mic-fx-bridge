#include "../src/PeakAccumulator.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
std::atomic<std::size_t> allocations { 0 };
int checks = 0;
int failures = 0;

void expect(bool condition, std::string_view message)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

void testQuietBlocksCannotEraseShortPeak()
{
    mic_daw::PeakAccumulator meter;
    const auto& uiMeter = meter;
    expect(uiMeter.take() == 0.0f, "new meter has no pending peak");

    meter.publish(1.0f);
    for (int block = 0; block < 9; ++block)
        meter.publish(0.125f);
    expect(uiMeter.take() == 1.0f,
           "one full-scale block survives nine quieter blocks before a UI tick");
    expect(uiMeter.take() == 0.0f, "taking a peak consumes that UI window");

    for (int block = 0; block < 10; ++block)
        meter.publish(0.25f);
    expect(uiMeter.take() == 0.25f, "a new quieter window does not retain the old maximum");
    expect(uiMeter.take() == 0.0f, "an empty next window is exact silence");

    for (const auto peak : { 0.125f, 0.5f, 0.25f, 0.375f, 0.0f })
        meter.publish(peak);
    expect(uiMeter.take() == 0.5f, "order and silent final blocks do not affect maximum");
}

void testSilenceResetAndInputValidation()
{
    mic_daw::PeakAccumulator meter;
    meter.publish(0.0f);
    meter.publish(-0.0f);
    expect(meter.take() == 0.0f, "positive and negative zero remain silence");

    meter.publish(0.75f);
    meter.publish(0.0f);
    expect(meter.take() == 0.75f, "publishing silence does not discard an earlier peak");

    meter.publish(0.75f);
    meter.reset();
    expect(meter.take() == 0.0f, "reset intentionally discards pending history");
    meter.publish(0.125f);
    expect(meter.take() == 0.125f, "publication works normally after reset");
    meter.reset();
    meter.reset();
    expect(meter.take() == 0.0f, "repeated resets are harmless");

    for (const auto invalid : { -1.0f, std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity() })
    {
        meter.publish(invalid);
        expect(meter.take() == 0.0f, "invalid metadata cannot poison an empty meter");
        meter.publish(0.5f);
        meter.publish(invalid);
        expect(meter.take() == 0.5f, "invalid metadata cannot erase a valid peak");
    }
}

void testOverFullScaleIsNotSaturated()
{
    mic_daw::PeakAccumulator meter;
    meter.publish(1.0f);
    expect(meter.take() == 1.0f, "full scale is preserved exactly");
    meter.publish(1.25f);
    meter.publish(0.5f);
    expect(meter.take() == 1.25f, "over-full-scale metadata is not clamped to one");

    const auto maximum = std::numeric_limits<float>::max();
    meter.publish(maximum);
    meter.publish(2.0f);
    expect(meter.take() == maximum, "the largest finite float is preserved without overflow");
    meter.publish(0.125f);
    expect(meter.take() == 0.125f, "large history does not contaminate the next window");
}

void testConcurrentProducerAndConsumer()
{
    // Synthetic metadata only. No microphone, audio device, or plugin is opened.
    mic_daw::PeakAccumulator meter;
    std::atomic<bool> start { false };
    std::atomic<bool> finished { false };
    std::thread producer([&]
    {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();

        for (int block = 0; block < 100000; ++block)
        {
            meter.publish(block == 12345 ? 2.0f : 0.125f);
            if (block % 64 == 0)
                std::this_thread::yield();
        }
        finished.store(true, std::memory_order_release);
    });

    const auto& uiMeter = meter;
    float observedMaximum = 0.0f;
    bool validReadings = true;
    start.store(true, std::memory_order_release);
    while (!finished.load(std::memory_order_acquire))
    {
        const auto peak = uiMeter.take();
        validReadings = validReadings
                        && (peak == 0.0f || peak == 0.125f || peak == 2.0f);
        observedMaximum = std::max(observedMaximum, peak);
        std::this_thread::yield();
    }
    producer.join();
    observedMaximum = std::max(observedMaximum, uiMeter.take());
    expect(validReadings, "concurrent reads contain only complete published magnitudes");
    expect(observedMaximum == 2.0f,
           "concurrent exchanges cannot lose a transient maximum amid quieter blocks");
    expect(uiMeter.take() == 0.0f, "final drain leaves no duplicate pending peak");
}

void testNoAllocation()
{
    const auto before = allocations.load(std::memory_order_relaxed);
    mic_daw::PeakAccumulator meter;
    for (int index = 0; index < 10000; ++index)
    {
        meter.publish(static_cast<float>(index % 17) * 0.125f);
        if (index % 10 == 0)
            static_cast<void>(meter.take());
        if (index % 137 == 0)
            meter.reset();
    }
    static_cast<void>(meter.take());
    const auto after = allocations.load(std::memory_order_relaxed);
    expect(after == before, "construction, publish, take and reset allocate no heap memory");
}
} // namespace

// Thread creation is outside the allocation check. The meter itself owns only
// one lock-free atomic float, with no storage allocation or synchronization lock.
void* operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

static_assert(noexcept(std::declval<mic_daw::PeakAccumulator&>().publish(1.0f)));
static_assert(noexcept(std::declval<const mic_daw::PeakAccumulator&>().take()));
static_assert(noexcept(std::declval<mic_daw::PeakAccumulator&>().reset()));

int main()
{
    testQuietBlocksCannotEraseShortPeak();
    testSilenceResetAndInputValidation();
    testOverFullScaleIsNotSaturated();
    testConcurrentProducerAndConsumer();
    testNoAllocation();
    std::cout << "Peak accumulator: " << checks << " checks, " << failures << " failures\n";
    std::cout << "Scope: peak display history only; audio samples and hardware ADC alignment were not tested.\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
