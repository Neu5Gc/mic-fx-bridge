#pragma once

#include <juce_core/juce_core.h>

namespace mic_daw
{
// Compatibility identifiers only. Never parse or apply the removed DSP profile.
inline bool isRetiredSetting(const juce::String& name)
{
    return name == "correctionProfileJson" || name == "correctionEnabled";
}

inline void stripRetiredSettings(juce::StringPairArray& properties)
{
    properties.remove("correctionProfileJson");
    properties.remove("correctionEnabled");
}

inline void stripRetiredSettings(juce::PropertySet& properties)
{
    properties.removeValue("correctionProfileJson");
    properties.removeValue("correctionEnabled");
}
} // namespace mic_daw
