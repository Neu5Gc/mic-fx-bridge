#include "../src/RmsMeter.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <string_view>
#include <vector>

namespace
{
std::atomic<std::size_t> allocations { 0 };
int failures = 0;
int checks = 0;

void expect(bool condition, std::string_view message)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

void expectNear(double actual, double expected, double tolerance, std::string_view message)
{
    expect(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

double dbfs(double amplitude)
{
    return amplitude > 0.0 ? 20.0 * std::log10(amplitude)
                           : -std::numeric_limits<double>::infinity();
}

void feedMono(mic_daw::RollingRmsMeter& meter, const std::vector<float>& samples,
              const std::vector<int>& blockSizes)
{
    std::size_t offset = 0;
    std::size_t nextBlock = 0;
    while (offset < samples.size())
    {
        const auto count = std::min(static_cast<std::size_t>(
                                        blockSizes[nextBlock++ % blockSizes.size()]),
                                    samples.size() - offset);
        const float* channels[] { samples.data() + offset };
        meter.process(channels, 1, static_cast<int>(count));
        offset += count;
    }
}

void testFullScaleAndSine()
{
    mic_daw::RollingRmsMeter meter;
    meter.prepare(48000.0);
    expect(meter.windowFrames() == 14400, "48 kHz window is exactly 300 ms");
    expect(meter.level() == 0.0f, "prepared meter starts at silence");

    std::vector<float> samples(14400, 1.0f);
    feedMono(meter, samples, { 512 });
    expectNear(dbfs(meter.level()), 0.0, 0.00001, "full-scale DC is 0 dBFS RMS");

    std::fill(samples.begin(), samples.end(), -1.0f);
    feedMono(meter, samples, { 127, 480, 1024 });
    expectNear(dbfs(meter.level()), 0.0, 0.00001, "negative full-scale DC is 0 dBFS RMS");

    for (std::size_t frame = 0; frame < samples.size(); ++frame)
        samples[frame] = static_cast<float>(std::sin(
            2.0 * std::numbers::pi * 1000.0 * static_cast<double>(frame) / 48000.0));
    meter.reset();
    feedMono(meter, samples, { 1, 7, 512, 480, 4096 });
    const auto fullScaleSineDb = dbfs(meter.level());
    expectNear(fullScaleSineDb, -3.0102999566, 0.00001,
               "full-scale sine has mathematical RMS -3.0103 dBFS");

    for (auto& sample : samples)
        sample *= 0.5f;
    feedMono(meter, samples, { 14400 });
    expectNear(dbfs(meter.level()) - fullScaleSineDb, -6.0205999133, 0.00001,
               "half amplitude lowers RMS by 6.0206 dB");

    std::fill(samples.begin(), samples.end(), 2.0f);
    feedMono(meter, samples, { 512 });
    expectNear(dbfs(meter.level()), 6.0205999133, 0.00001,
               "meter does not clip floating-point levels above 0 dBFS");
}

void testChannelEnergy()
{
    mic_daw::RollingRmsMeter meter;
    meter.prepare(100.0);
    std::array<float, 30> positive;
    std::array<float, 30> negative;
    std::array<float, 30> silence {};
    positive.fill(0.5f);
    negative.fill(-0.5f);

    const float* mono[] { positive.data() };
    meter.process(mono, 1, 30);
    const auto monoDb = dbfs(meter.level());

    const float* duplicate[] { positive.data(), positive.data() };
    meter.process(duplicate, 2, 30);
    expectNear(dbfs(meter.level()), monoDb, 0.00001,
               "mono duplicated to stereo has no artificial +3 dB gain");

    const float* opposite[] { positive.data(), negative.data() };
    meter.process(opposite, 2, 30);
    expectNear(dbfs(meter.level()), monoDb, 0.00001,
               "opposite-phase channels do not cancel in an energy meter");

    const float* oneSilent[] { positive.data(), silence.data() };
    meter.process(oneSilent, 2, 30);
    expectNear(dbfs(meter.level()) - monoDb, -3.0102999566, 0.00001,
               "one silent active stereo channel reduces mean-square by half");

    const float* inactive[] { positive.data(), nullptr };
    meter.process(inactive, 2, 30);
    expectNear(dbfs(meter.level()), monoDb, 0.00001,
               "null inactive channels are excluded from channel averaging");

    const float* fourChannels[] { positive.data(), positive.data(),
                                  silence.data(), silence.data() };
    meter.process(fourChannels, 4, 30);
    expectNear(dbfs(meter.level()) - monoDb, -3.0102999566, 0.00001,
               "actual active output padding channels are included as silence");
}

void testWindowDecayAndReset()
{
    mic_daw::RollingRmsMeter meter;
    meter.prepare(100.0);
    std::array<float, 30> ones;
    ones.fill(1.0f);
    const float* channels[] { ones.data() };

    meter.process(channels, 1, 15);
    expectNear(meter.level(), std::sqrt(0.5), 0.0000001,
               "startup window is padded with silence, independent of block size");
    meter.process(channels, 1, 15);
    expectNear(meter.level(), 1.0, 0.0000001, "a full window reaches steady RMS");
    meter.processSilence(15);
    expectNear(meter.level(), std::sqrt(0.5), 0.0000001,
               "half-window silence removes exactly half of the previous energy");
    meter.processSilence(14);
    expectNear(meter.level(), std::sqrt(1.0 / 30.0), 0.0000001,
               "last nonzero sample remains until exactly 300 ms passes");
    meter.processSilence(1);
    expect(meter.level() == 0.0f, "all-silent window has exactly zero, no residual energy");

    meter.process(channels, 1, 30);
    meter.reset();
    expect(meter.level() == 0.0f, "reset immediately clears displayed RMS");
    meter.process(channels, 1, 1);
    expectNear(meter.level(), std::sqrt(1.0 / 30.0), 0.0000001,
               "new history after reset does not subtract stale ring contents");
    meter.processSilence(30);
    expect(meter.level() == 0.0f, "whole silent block uses exact constant-time reset");

    meter.process(channels, 1, 30);
    meter.prepare(200.0);
    expect(meter.windowFrames() == 60 && meter.level() == 0.0f,
           "new sample rate clears prior device history and resizes window");
}

void testVariedBlocksAndRates()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto totalFrames = static_cast<std::size_t>(std::round(rate * 0.91));
        std::vector<float> samples(totalFrames);
        for (std::size_t frame = 0; frame < totalFrames; ++frame)
        {
            const auto time = static_cast<double>(frame) / rate;
            samples[frame] = static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 197.0 * time)
                                                + 0.1 * std::cos(2.0 * std::numbers::pi * 3173.0 * time));
        }

        mic_daw::RollingRmsMeter oneBlock;
        mic_daw::RollingRmsMeter manyBlocks;
        oneBlock.prepare(rate);
        manyBlocks.prepare(rate);
        feedMono(oneBlock, samples, { static_cast<int>(totalFrames) });
        feedMono(manyBlocks, samples, { 1, 3, 32, 511, 512, 480, 1024, 4096 });
        double expectedEnergy = 0.0;
        for (std::size_t frame = totalFrames - oneBlock.windowFrames(); frame < totalFrames; ++frame)
        {
            const auto sample = static_cast<double>(samples[frame]);
            expectedEnergy += sample * sample;
        }
        const auto expected = std::sqrt(expectedEnergy
                                        / static_cast<double>(oneBlock.windowFrames()));
        expectNear(oneBlock.level(), expected, 0.0000001,
                   "rolling RMS matches direct last-window integration at each sample rate");
        expect(oneBlock.level() == manyBlocks.level(),
               "RMS is bit-identical across different callback partitions");
    }
}

void testInvalidAndNonfiniteInput()
{
    mic_daw::RollingRmsMeter meter;
    const float* empty[] { nullptr, nullptr };
    meter.process(empty, 2, 64);
    expect(meter.level() == 0.0f, "unprepared meter safely reports silence");
    meter.prepare(std::numeric_limits<double>::quiet_NaN());
    expect(meter.windowFrames() == 14400, "NaN sample rate uses bounded 48 kHz fallback");
    meter.prepare(std::numeric_limits<double>::infinity());
    expect(meter.windowFrames() == 14400, "infinite sample rate uses bounded fallback");
    meter.prepare(-48000.0);
    expect(meter.windowFrames() == 14400, "negative sample rate uses fallback");
    meter.prepare(0.0);
    expect(meter.windowFrames() == 14400, "zero sample rate uses fallback");
    meter.prepare(1.0e100);
    expect(meter.windowFrames() == 230400, "extreme sample rate allocation is bounded");
    meter.prepare(1.0);
    expect(meter.windowFrames() == 1, "tiny positive sample rate uses at least one frame");

    meter.prepare(100.0);
    std::array<float, 30> samples;
    samples.fill(1.0f);
    samples[0] = std::numeric_limits<float>::quiet_NaN();
    samples[1] = std::numeric_limits<float>::infinity();
    samples[2] = -std::numeric_limits<float>::infinity();
    const float* channels[] { samples.data() };
    meter.process(channels, 1, 30);
    expectNear(meter.level(), std::sqrt(27.0 / 30.0), 0.0000001,
               "nonfinite samples count as zero energy without poisoning RMS");
    expect(std::isnan(samples[0]) && std::isinf(samples[1]) && std::isinf(samples[2])
               && samples[3] == 1.0f,
           "sanitising the meter never changes signal samples");
    meter.process(channels, 1, 0);
    meter.process(channels, 1, -1);
    expectNear(meter.level(), std::sqrt(27.0 / 30.0), 0.0000001,
               "empty or invalid blocks do not advance history");
    meter.process(empty, 2, 30);
    expect(meter.level() == 0.0f, "all inactive channels advance a silent window");
    samples.fill(1.0f);
    meter.process(channels, 1, 30);
    meter.process(nullptr, 2, 30);
    expect(meter.level() == 0.0f, "null buffer array safely advances silence");
    meter.process(channels, 1, 30);
    meter.process(channels, 0, 30);
    expect(meter.level() == 0.0f, "zero active channels safely advance silence");
}

void testLongRunningSilenceAndNoAllocation()
{
    mic_daw::RollingRmsMeter meter;
    meter.prepare(48000.0);
    std::array<float, 1024> samples;
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
        samples[frame] = static_cast<float>(static_cast<double>(frame % 23) / 23.0 - 0.5);
    const float* channels[] { samples.data(), samples.data() };

    const auto before = allocations.load();
    for (int callback = 0; callback < 10000; ++callback)
    {
        meter.process(channels, 2, (callback % 1024) + 1);
        static_cast<void>(meter.level());
        if (callback % 73 == 0)
            meter.processSilence(300);
        if (callback % 137 == 0)
            meter.reset();
    }
    for (int callback = 0; callback < 30; ++callback)
        meter.processSilence(512);
    const auto after = allocations.load();
    expect(after == before, "processing, reading and resetting allocate nothing after prepare");
    expect(meter.level() == 0.0f, "long-running meter decays to exact silence without accumulated drift");
}
} // namespace

// Count all ordinary C++ allocations, including vector allocations. The meter
// uses no over-aligned storage. Preparation is deliberately outside the guard.
void* operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(std::max<std::size_t>(size, 1)))
        return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main()
{
    testFullScaleAndSine();
    testChannelEnergy();
    testWindowDecayAndReset();
    testVariedBlocksAndRates();
    testInvalidAndNonfiniteInput();
    testLongRunningSilenceAndNoAllocation();
    std::cout << "RMS meter: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
