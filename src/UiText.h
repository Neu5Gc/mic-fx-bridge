#pragma once

#include <juce_core/juce_core.h>

#ifndef MIC_VST_BRIDGE_ENGLISH
#define MIC_VST_BRIDGE_ENGLISH 1
#endif

namespace mic_daw
{
// New English-only product copy keeps the same explicit UTF-8 conversion.
[[nodiscard]] inline juce::String uiText(const char* english)
{
    return juce::String::fromUTF8(english);
}

[[nodiscard]] inline juce::String uiText(const char8_t* korean,
                                         const char* english)
{
#if MIC_VST_BRIDGE_ENGLISH
    static_cast<void>(korean);
    return juce::String::fromUTF8(english);
#else
    static_cast<void>(english);
    return juce::String(korean);
#endif
}
} // namespace mic_daw
