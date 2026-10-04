#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

namespace mic_daw
{
inline juce::String formatDbfs(float magnitude)
{
    if (!std::isfinite(magnitude) || magnitude <= 0.0f)
        return juce::String(u8"−∞ dBFS");
    return juce::String(20.0 * std::log10(static_cast<double>(magnitude)), 1) + " dBFS";
}

inline juce::String formatPeakAndRms(float peak, float rms)
{
    return "PK " + formatDbfs(peak) + "\nRMS " + formatDbfs(rms);
}
} // namespace mic_daw
