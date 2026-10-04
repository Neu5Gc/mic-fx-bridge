#pragma once

namespace mic_daw
{
struct SelectedInputChannels
{
    const float* left = nullptr;
    const float* right = nullptr;

    [[nodiscard]] constexpr bool hasAudioData() const noexcept
    {
        return left != nullptr && right != nullptr;
    }
};

// The existing InputCallback routing, extracted without gain or DSP changes.
// The caller still validates a positive frame count and device authorization.
// 0 selects input 1 and duplicates it; 1 selects input 2 and duplicates it;
// all other values preserve inputs 1/2 as stereo (the public setter clamps to
// 0..2). A missing pointer falls back to the other first/second input only.
// A present pointer containing silence, NaN, or any other sample never falls
// back: selection does not read, combine, scale, sanitize, or write samples.
[[nodiscard]] constexpr SelectedInputChannels selectInputChannels(
    const float* const* inputChannelData, int numInputChannels, int mode) noexcept
{
    if (inputChannelData == nullptr || numInputChannels <= 0)
        return {};

    const auto* first = inputChannelData[0];
    const auto* second = numInputChannels > 1 ? inputChannelData[1] : nullptr;

    if (first == nullptr && second == nullptr)
        return {};

    if (first == nullptr)
        first = second;
    if (second == nullptr)
        second = first;

    if (mode == 0)
        return { first, first };
    if (mode == 1)
        return { second, second };
    return { first, second };
}
} // namespace mic_daw
