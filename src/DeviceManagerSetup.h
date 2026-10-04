#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace mic_daw
{
// initialise(2, 0, nullptr, false) still opens the system's default input.
// Zero channels initialise enumeration only; restore the chosen endpoint first.
inline juce::String initialiseWithoutOpeningDevice(juce::AudioDeviceManager& manager)
{
    return manager.initialise(0, 0, nullptr, false);
}

inline juce::AudioDeviceManager::AudioDeviceSetup makeInputDeviceSetup(
    const juce::String& name, double sampleRate)
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = name;
    setup.sampleRate = sampleRate;
    setup.bufferSize = 0;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.setRange(0, 2, true);
    return setup;
}

inline juce::AudioDeviceManager::AudioDeviceSetup makeOutputDeviceSetup(
    const juce::String& name, double sampleRate)
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = name;
    setup.sampleRate = sampleRate;
    setup.bufferSize = 0;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.setRange(0, 2, true);
    return setup;
}
} // namespace mic_daw
