#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mic_daw
{
// A rectangular, sample-accurate 300 ms energy window, not an average of
// per-block RMS values. Before a complete window arrives, its missing history
// is silence. Only prepare() allocates; one audio callback owns process/reset.
class RollingRmsMeter final
{
public:
    static constexpr double windowSeconds = 0.3;
    static constexpr double maximumSampleRate = 768000.0;

    void prepare(double sampleRate)
    {
        const auto validRate = std::isfinite(sampleRate) && sampleRate > 0.0
                                   ? std::min(sampleRate, maximumSampleRate)
                                   : 48000.0;
        const auto frames = static_cast<std::size_t>(
            std::max(1.0, std::round(validRate * windowSeconds)));
        energyHistory.assign(frames, 0.0);
        reset();
    }

    // Lazy invalidation makes disconnects/resets constant-time, without
    // touching or reallocating the sample history from the real-time thread.
    void reset() noexcept
    {
        writeIndex = 0;
        validFrames = 0;
        nonzeroFrames = 0;
        energySum = 0.0;
        summationCorrection = 0.0;
    }

    void process(const float* const* channels, int numChannels, int numFrames) noexcept
    {
        if (numFrames <= 0 || energyHistory.empty())
            return;

        int activeChannels = 0;
        if (channels != nullptr)
            for (int channel = 0; channel < numChannels; ++channel)
                if (channels[channel] != nullptr)
                    ++activeChannels;

        if (activeChannels == 0)
        {
            processSilence(numFrames);
            return;
        }

        // Null pointers are inactive channels. A present, silent channel does
        // count in the mean, so mono duplicated to stereo keeps its RMS while
        // a real stereo signal with one silent side has 3.01 dB less energy.
        const auto channelScale = 1.0 / static_cast<double>(activeChannels);
        for (int frame = 0; frame < numFrames; ++frame)
        {
            double energy = 0.0;
            for (int channel = 0; channel < numChannels; ++channel)
            {
                if (channels[channel] == nullptr)
                    continue;

                const auto sample = static_cast<double>(channels[channel][frame]);
                // Meter sanitisation only: never modify the audio buffers.
                if (std::isfinite(sample))
                    energy += sample * sample;
            }

            appendEnergy(energy * channelScale);
        }
    }

    void processSilence(int numFrames) noexcept
    {
        if (numFrames <= 0 || energyHistory.empty())
            return;

        if (static_cast<std::size_t>(numFrames) >= energyHistory.size())
        {
            reset();
            return;
        }

        for (int frame = 0; frame < numFrames; ++frame)
            appendEnergy(0.0);
    }

    [[nodiscard]] float level() const noexcept
    {
        if (energyHistory.empty() || nonzeroFrames == 0)
            return 0.0f;

        const auto meanSquare = std::max(0.0, energySum)
                                / static_cast<double>(energyHistory.size());
        return static_cast<float>(std::sqrt(meanSquare));
    }

    [[nodiscard]] std::size_t windowFrames() const noexcept
    {
        return energyHistory.size();
    }

private:
    void addToSum(double amount) noexcept
    {
        // Compensated summation limits drift across long-running windows.
        const auto corrected = amount - summationCorrection;
        const auto next = energySum + corrected;
        summationCorrection = (next - energySum) - corrected;
        energySum = next;
    }

    void appendEnergy(double energy) noexcept
    {
        if (validFrames == energyHistory.size())
        {
            const auto previous = energyHistory[writeIndex];
            if (previous > 0.0)
                --nonzeroFrames;
            addToSum(-previous);
        }
        else
        {
            ++validFrames;
        }

        energyHistory[writeIndex] = energy;
        if (energy > 0.0)
            ++nonzeroFrames;
        addToSum(energy);

        if (++writeIndex == energyHistory.size())
            writeIndex = 0;

        // An all-silent window must be exactly zero, not a floating-point
        // residue that would leave a spurious dBFS readout after a sound ends.
        if (nonzeroFrames == 0)
        {
            energySum = 0.0;
            summationCorrection = 0.0;
        }
    }

    std::vector<double> energyHistory;
    std::size_t writeIndex = 0;
    std::size_t validFrames = 0;
    std::size_t nonzeroFrames = 0;
    double energySum = 0.0;
    double summationCorrection = 0.0;
};
} // namespace mic_daw
