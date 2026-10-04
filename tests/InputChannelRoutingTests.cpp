#include "../src/InputChannelRouting.h"
#include "../src/RmsMeter.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <string_view>
#include <vector>

namespace
{
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

void expectNear(double actual, double expected, double tolerance, std::string_view message)
{
    expect(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

std::uint32_t bits(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

double rms(const float* data, std::size_t count)
{
    double energy = 0.0;
    for (std::size_t frame = 0; frame < count; ++frame)
    {
        const auto sample = static_cast<double>(data[frame]);
        energy += sample * sample;
    }
    return std::sqrt(energy / static_cast<double>(count));
}

double gainDb(double outputRms, double inputRms)
{
    return 20.0 * std::log10(outputRms / inputRms);
}

double selectedRms(mic_daw::SelectedInputChannels selected, int frameCount, double sampleRate)
{
    mic_daw::RollingRmsMeter meter;
    meter.prepare(sampleRate);
    const float* channels[] { selected.left, selected.right };
    meter.process(channels, 2, frameCount);
    return meter.level();
}

void testPointerSelectionAndSampleIdentity()
{
    const std::array<std::uint32_t, 12> patterns {
        0x00000000u, 0x80000000u, // Positive and negative zero.
        0x3f800000u, 0xbf800000u, // +/-1.
        0x3eaaaaabu, 0xbeaaaaabu, // Non-exact fractions.
        0x7f800000u, 0xff800000u, // +/-infinity.
        0x7fc12345u, 0xffc54321u, // Quiet NaNs with distinct payloads/signs.
        0x00000001u, 0x7f7fffffu  // Smallest subnormal and largest finite float.
    };
    std::array<float, patterns.size()> first {};
    std::array<float, patterns.size()> second {};
    for (std::size_t frame = 0; frame < patterns.size(); ++frame)
    {
        first[frame] = std::bit_cast<float>(patterns[frame]);
        second[frame] = std::bit_cast<float>(patterns[patterns.size() - frame - 1]);
    }
    const float* inputs[] { first.data(), second.data() };

    const auto monoFirst = mic_daw::selectInputChannels(inputs, 2, 0);
    const auto monoSecond = mic_daw::selectInputChannels(inputs, 2, 1);
    const auto stereo = mic_daw::selectInputChannels(inputs, 2, 2);
    expect(monoFirst.hasAudioData() && monoFirst.left == first.data()
               && monoFirst.right == first.data(),
           "input 1 mono returns the input pointer twice, not a scaled copy");
    expect(monoSecond.hasAudioData() && monoSecond.left == second.data()
               && monoSecond.right == second.data(),
           "input 2 mono returns the input pointer twice, not a mix");
    expect(stereo.hasAudioData() && stereo.left == first.data()
               && stereo.right == second.data(),
           "stereo returns the original ordered input pointers");

    for (std::size_t frame = 0; frame < patterns.size(); ++frame)
    {
        expect(bits(first[frame]) == patterns[frame], "routing does not modify input 1 bits");
        expect(bits(second[frame]) == patterns[patterns.size() - frame - 1],
               "routing does not modify input 2 bits");
        expect(bits(monoFirst.left[frame]) == patterns[frame]
                   && bits(monoFirst.right[frame]) == patterns[frame],
               "input 1 duplication preserves every sample bit including NaN payloads");
        expect(bits(monoSecond.left[frame]) == patterns[patterns.size() - frame - 1]
                   && bits(monoSecond.right[frame]) == patterns[patterns.size() - frame - 1],
               "input 2 duplication preserves every sample bit including signed zero");
        expect(bits(stereo.left[frame]) == patterns[frame]
                   && bits(stereo.right[frame]) == patterns[patterns.size() - frame - 1],
               "stereo neither downmixes nor sanitizes either source channel");
    }
}

void testMissingInputsAndFallback()
{
    const std::array<float, 4> source { 0.125f, -0.25f, 0.375f, -0.5f };
    const std::array<float, 4> other { 0.5f, -0.375f, 0.25f, -0.125f };
    const float* singleInput[] { source.data() };
    const float* firstMissing[] { nullptr, source.data() };
    const float* secondMissing[] { source.data(), nullptr };
    const float* bothMissing[] { nullptr, nullptr };

    for (const auto mode : { 0, 1, 2 })
    {
        const auto single = mic_daw::selectInputChannels(singleInput, 1, mode);
        expect(single.hasAudioData() && single.left == source.data() && single.right == source.data(),
               "a one-channel device duplicates its sole source for every existing mode");
        const auto missingFirst = mic_daw::selectInputChannels(firstMissing, 2, mode);
        expect(missingFirst.hasAudioData() && missingFirst.left == source.data()
                   && missingFirst.right == source.data(),
               "null input 1 falls back to present input 2 without attenuation");
        const auto missingSecond = mic_daw::selectInputChannels(secondMissing, 2, mode);
        expect(missingSecond.hasAudioData() && missingSecond.left == source.data()
                   && missingSecond.right == source.data(),
               "null input 2 falls back to present input 1 without attenuation");
        expect(!mic_daw::selectInputChannels(bothMissing, 2, mode).hasAudioData(),
               "both missing source pointers produce no usable route");
        expect(!mic_daw::selectInputChannels(nullptr, 2, mode).hasAudioData(),
               "null channel array produces no usable route");
        expect(!mic_daw::selectInputChannels(singleInput, 0, mode).hasAudioData(),
               "zero channels never dereference the array");
        expect(!mic_daw::selectInputChannels(singleInput, -1, mode).hasAudioData(),
               "negative channel count is rejected without dereferencing the array");
    }

    const float* twoInputs[] { source.data(), other.data() };
    for (const auto mode : { -1, 3, 100 })
    {
        const auto selected = mic_daw::selectInputChannels(twoInputs, 2, mode);
        expect(selected.left == source.data() && selected.right == other.data(),
               "out-of-contract mode preserves the callback's existing stereo fallback");
    }

    const float* fourInputs[] { nullptr, nullptr, source.data(), other.data() };
    expect(!mic_daw::selectInputChannels(fourInputs, 4, 0).hasAudioData(),
           "existing routing never substitutes input 3/4 for missing input 1/2");
}

void testSilenceAndNonfiniteDoNotTriggerFallback()
{
    const std::array<float, 4> silence {};
    const std::array<float, 4> voice { 0.125f, -0.25f, 0.375f, -0.5f };
    const std::array<float, 4> nonfinite {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -0.0f
    };
    const float* silentFirst[] { silence.data(), voice.data() };
    const auto chosenSilent = mic_daw::selectInputChannels(silentFirst, 2, 0);
    expect(chosenSilent.left == silence.data() && chosenSilent.right == silence.data(),
           "valid silent input 1 does not fall back to audible input 2");
    expect(rms(chosenSilent.left, silence.size()) == 0.0,
           "choosing a present silent channel remains silence");

    const float* silentSecond[] { voice.data(), silence.data() };
    const auto secondSilent = mic_daw::selectInputChannels(silentSecond, 2, 1);
    expect(secondSilent.left == silence.data() && secondSilent.right == silence.data(),
           "valid silent input 2 does not fall back to audible input 1");

    const float* invalidSamples[] { nonfinite.data(), voice.data() };
    const auto selected = mic_daw::selectInputChannels(invalidSamples, 2, 0);
    expect(selected.left == nonfinite.data() && selected.right == nonfinite.data(),
           "nonfinite samples are not treated as a missing channel pointer");
    expect(std::isnan(selected.left[0]) && std::isinf(selected.left[1])
               && bits(selected.left[3]) == bits(-0.0f),
           "source NaN, infinity and signed zero survive unchanged through routing");
}

void testUnityRmsForEachChannelAndSampleRate()
{
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto frameCount = static_cast<int>(std::round(sampleRate * 0.3));
        std::vector<float> first(static_cast<std::size_t>(frameCount));
        std::vector<float> second(static_cast<std::size_t>(frameCount));
        for (int frame = 0; frame < frameCount; ++frame)
        {
            const auto time = static_cast<double>(frame) / sampleRate;
            first[static_cast<std::size_t>(frame)] = static_cast<float>(
                0.43 * std::sin(2.0 * std::numbers::pi * 200.0 * time));
            second[static_cast<std::size_t>(frame)] = static_cast<float>(
                0.21 * std::cos(2.0 * std::numbers::pi * 470.0 * time) + 0.013);
        }
        const float* inputs[] { first.data(), second.data() };
        const auto firstRms = rms(first.data(), first.size());
        const auto secondRms = rms(second.data(), second.size());
        const auto monoFirst = mic_daw::selectInputChannels(inputs, 2, 0);
        const auto monoSecond = mic_daw::selectInputChannels(inputs, 2, 1);
        const auto stereo = mic_daw::selectInputChannels(inputs, 2, 2);
        expectNear(gainDb(selectedRms(monoFirst, frameCount, sampleRate), firstRms),
                   0.0, 0.00001, "input 1 -> dual mono has exactly 0 dB gain at every rate");
        expectNear(gainDb(selectedRms(monoSecond, frameCount, sampleRate), secondRms),
                   0.0, 0.00001, "input 2 -> dual mono has exactly 0 dB gain at every rate");
        expectNear(gainDb(rms(stereo.left, first.size()), firstRms), 0.0, 0.00001,
                   "stereo left preserves source-channel RMS at unity");
        expectNear(gainDb(rms(stereo.right, second.size()), secondRms), 0.0, 0.00001,
                   "stereo right preserves source-channel RMS at unity");
        const auto expectedStereoRms = std::sqrt((firstRms * firstRms + secondRms * secondRms) / 2.0);
        expectNear(gainDb(selectedRms(stereo, frameCount, sampleRate), expectedStereoRms),
                   0.0, 0.00001, "stereo combined RMS matches unchanged input-channel mean energy");
    }
}

void testSingleSidedVoiceVersusExternalDownmix()
{
    constexpr int frames = 14400;
    constexpr double sampleRate = 48000.0;
    std::vector<float> voice(frames);
    std::vector<float> silence(frames, 0.0f);
    std::vector<float> externalMono(frames);
    for (int frame = 0; frame < frames; ++frame)
        voice[static_cast<std::size_t>(frame)] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 200.0 * static_cast<double>(frame) / sampleRate));
    const auto sourceRms = rms(voice.data(), voice.size());

    for (const auto voiceChannel : { 0, 1 })
    {
        const float* inputs[] { voiceChannel == 0 ? voice.data() : silence.data(),
                                voiceChannel == 1 ? voice.data() : silence.data() };
        const auto mono = mic_daw::selectInputChannels(inputs, 2, voiceChannel);
        const auto stereo = mic_daw::selectInputChannels(inputs, 2, 2);
        expectNear(gainDb(selectedRms(mono, frames, sampleRate), sourceRms), 0.0, 0.00001,
                   "selecting the audible channel as mono preserves its level");
        expectNear(gainDb(selectedRms(stereo, frames, sampleRate), sourceRms),
                   -3.0102999566, 0.00001,
                   "single-sided stereo mean RMS is -3.01 dB without changing the voice samples");
        const auto* audibleSide = voiceChannel == 0 ? stereo.left : stereo.right;
        expectNear(gainDb(rms(audibleSide, voice.size()), sourceRms), 0.0, 0.00001,
                   "the audible stereo side itself still has unity gain");

        // This is an explicit hypothetical downstream mix, not processing
        // performed by selectInputChannels or evidence about a running app.
        for (int frame = 0; frame < frames; ++frame)
            externalMono[static_cast<std::size_t>(frame)] =
                0.5f * (stereo.left[frame] + stereo.right[frame]);
        expectNear(gainDb(rms(externalMono.data(), externalMono.size()), sourceRms),
                   -6.0205999133, 0.00001,
                   "a later (L+R)/2 downmix would reduce a one-sided stereo voice by -6.02 dB");

        for (int frame = 0; frame < frames; ++frame)
            externalMono[static_cast<std::size_t>(frame)] =
                0.5f * (mono.left[frame] + mono.right[frame]);
        expectNear(gainDb(rms(externalMono.data(), externalMono.size()), sourceRms),
                   0.0, 0.00001, "dual-mono selection remains unity after the same hypothetical downmix");
    }
}
} // namespace

int main()
{
    testPointerSelectionAndSampleIdentity();
    testMissingInputsAndFallback();
    testSilenceAndNonfiniteDoNotTriggerFallback();
    testUnityRmsForEachChannelAndSampleRate();
    testSingleSidedVoiceVersusExternalDownmix();
    std::cout << "Input channel routing: " << checks << " checks, " << failures << " failures\n";
    std::cout << "Scope: source-pointer routing and metering only; no devices, FIFO, VSTs or external app were exercised.\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
