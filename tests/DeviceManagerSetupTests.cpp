#include "../src/DeviceManagerSetup.h"
#include "../src/MeterReadout.h"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace
{
int checks = 0;
int failures = 0;
void expect(bool condition, const char* message)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

struct Calls
{
    int creates = 0, opens = 0, starts = 0;
    juce::String input, output;
    juce::BigInteger inputChannels, outputChannels;
};

class FakeDevice final : public juce::AudioIODevice
{
public:
    FakeDevice(Calls& callsToUse, juce::String input, juce::String output)
        : AudioIODevice(input.isNotEmpty() ? input : output, "Fake WASAPI"),
          inputName(std::move(input)), outputName(std::move(output)), calls(callsToUse) {}
    ~FakeDevice() override { stop(); }
    juce::StringArray getInputChannelNames() override { return { "Input 1", "Input 2" }; }
    juce::StringArray getOutputChannelNames() override
    {
        juce::StringArray names;
        for (int channel = 1; channel <= 16; ++channel) names.add("Output " + juce::String(channel));
        return names;
    }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 512 }; }
    int getDefaultBufferSize() override { return 512; }
    juce::String open(const juce::BigInteger& inputs, const juce::BigInteger& outputs,
                      double, int) override
    {
        ++calls.opens;
        calls.input = inputName; calls.output = outputName;
        calls.inputChannels = activeInputs = inputs;
        calls.outputChannels = activeOutputs = outputs;
        opened = true;
        return {};
    }
    void close() override { stop(); opened = false; }
    bool isOpen() override { return opened; }
    void start(juce::AudioIODeviceCallback* next) override
    {
        ++calls.starts; callback = next;
        if (callback != nullptr) callback->audioDeviceAboutToStart(this);
    }
    void stop() override
    {
        if (callback != nullptr) { auto* old = callback; callback = nullptr; old->audioDeviceStopped(); }
    }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return 512; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveInputChannels() const override { return activeInputs; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
    int getInputLatencyInSamples() override { return 0; }
    int getOutputLatencyInSamples() override { return 0; }

    const juce::String inputName, outputName;
private:
    Calls& calls;
    juce::BigInteger activeInputs, activeOutputs;
    juce::AudioIODeviceCallback* callback = nullptr;
    bool opened = false;
};

class FakeType final : public juce::AudioIODeviceType
{
public:
    explicit FakeType(Calls& callsToUse) : AudioIODeviceType("Fake WASAPI"), calls(callsToUse) {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool input) const override
    {
        return input ? juce::StringArray { "Default microphone", "Selected microphone" }
                     : juce::StringArray { "Default speakers", "Selected cable" };
    }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice* device, bool input) const override
    {
        auto* fake = dynamic_cast<FakeDevice*>(device);
        return fake == nullptr ? -1 : getDeviceNames(input).indexOf(input ? fake->inputName : fake->outputName);
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice(const juce::String& output, const juce::String& input) override
    {
        ++calls.creates;
        return new FakeDevice(calls, input, output);
    }
private:
    Calls& calls;
};

class FakeManager final : public juce::AudioDeviceManager
{
public:
    explicit FakeManager(Calls& callsToUse) : calls(callsToUse) {}
private:
    void createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) override
    {
        types.add(new FakeType(calls));
    }
    Calls& calls;
};

class LifecycleCallback final : public juce::AudioIODeviceCallback
{
public:
    void audioDeviceIOCallbackWithContext(
        const float* const*, int,
        float* const*, int, int,
        const juce::AudioIODeviceCallbackContext&) override
    {
    }

    void audioDeviceAboutToStart(juce::AudioIODevice*) override
    {
        ++aboutToStartCalls;
    }

    void audioDeviceStopped() override
    {
        ++stoppedCalls;
    }

    void audioDeviceError(const juce::String&) override {}

    int aboutToStartCalls = 0;
    int stoppedCalls = 0;
};

void testCallbackDetachAttachRepreparesActiveDevice()
{
    Calls calls;
    FakeManager manager(calls);
    expect(mic_daw::initialiseWithoutOpeningDevice(manager).isEmpty(),
           "callback lifecycle manager initialises");

    LifecycleCallback callback;
    manager.addAudioCallback(&callback);
    expect(manager.setAudioDeviceSetup(
               mic_daw::makeInputDeviceSetup("Selected microphone", 48000.0), true).isEmpty(),
           "callback lifecycle input opens");
    expect(callback.aboutToStartCalls == 1,
           "opening the input prepares the registered callback");

    // Mic-correction profile/enable changes use exactly this JUCE lifecycle:
    // removal drains the callback and calls stopped; re-addition to an active
    // manager immediately calls aboutToStart before audio processing resumes.
    manager.removeAudioCallback(&callback);
    expect(callback.stoppedCalls == 1,
           "detaching from an active input stops the callback state");
    manager.addAudioCallback(&callback);
    expect(callback.aboutToStartCalls == 2,
           "reattaching to an active input re-prepares correction state");

    manager.removeAudioCallback(&callback);
    expect(callback.stoppedCalls == 2,
           "final callback detach stops the reprepared state");
    manager.closeAudioDevice();
}

void testNoDefaultOpen()
{
    // Reproduce the old startup side effect using fake devices only.
    Calls legacyCalls;
    {
        FakeManager legacy(legacyCalls);
        expect(legacy.initialise(2, 0, nullptr, false).isEmpty(), "old setup initialises");
        expect(legacyCalls.opens == 1 && legacyCalls.input == "Default microphone",
               "old initialise opens the unselected default microphone");
        expect(legacyCalls.starts == 1, "old initialise also starts the default stream");
    }

    Calls calls;
    FakeManager manager(calls);
    expect(mic_daw::initialiseWithoutOpeningDevice(manager).isEmpty(), "enumeration initialises");
    expect(calls.creates == 0 && calls.opens == 0 && calls.starts == 0,
           "new startup never creates, opens, or starts an endpoint");
    expect(manager.getCurrentAudioDevice() == nullptr, "new startup owns no device");

    const auto setup = mic_daw::makeInputDeviceSetup("Selected microphone", 48000.0);
    for (int iteration = 0; iteration < 5; ++iteration)
    {
        expect(manager.setAudioDeviceSetup(setup, true).isEmpty(), "explicit input opens");
        expect(calls.input == "Selected microphone" && calls.output.isEmpty(), "only requested input is used");
        expect(calls.inputChannels.toInteger() == 3 && calls.outputChannels.isZero(), "exact first two inputs, no outputs");
        manager.closeAudioDevice();
    }
    expect(calls.opens == 5 && calls.starts == 5, "five reconnects have exactly five stream opens");
    const auto beforeMissing = calls.opens;
    expect(manager.setAudioDeviceSetup(mic_daw::makeInputDeviceSetup("Missing microphone", 48000.0), true).isNotEmpty(),
           "missing input reports failure");
    expect(calls.opens == beforeMissing, "missing input never falls back to default");

    const auto output = mic_daw::makeOutputDeviceSetup("Selected cable", 48000.0);
    expect(manager.setAudioDeviceSetup(output, true).isEmpty(), "explicit output opens");
    expect(calls.input.isEmpty() && calls.output == "Selected cable", "output setup has no microphone");
    expect(calls.inputChannels.isZero() && calls.outputChannels.toInteger() == 3,
           "16-channel output still enables exactly two channels");
}

void testPcmFullScaleConversion()
{
    // Use the same JUCE converter types as the WASAPI backend, not a gain model.
    using Dest = juce::AudioData::Pointer<juce::AudioData::Float32, juce::AudioData::NativeEndian,
                                         juce::AudioData::NonInterleaved, juce::AudioData::NonConst>;
    using Int16Source = juce::AudioData::Pointer<juce::AudioData::Int16, juce::AudioData::LittleEndian,
                                                juce::AudioData::Interleaved, juce::AudioData::Const>;
    using Int32Source = juce::AudioData::Pointer<juce::AudioData::Int32, juce::AudioData::LittleEndian,
                                                juce::AudioData::Interleaved, juce::AudioData::Const>;
    using FloatSource = juce::AudioData::Pointer<juce::AudioData::Float32, juce::AudioData::LittleEndian,
                                                juce::AudioData::Interleaved, juce::AudioData::Const>;
    const std::int16_t pcm16[] { -32768, 0, 16384, 0, 32767, 0 };
    const std::int32_t pcm24in32[] { (-2147483647 - 1), 0, 1073741824, 0, 2147483392, 0 };
    float converted[3] {};
    juce::AudioData::ConverterInstance<Int16Source, Dest> int16Converter(2, 1);
    int16Converter.convertSamples(converted, 0, pcm16, 0, 3);
    expect(converted[0] == -1.0f && converted[1] == 0.5f, "PCM16 negative full scale and half scale are preserved");
    expect(converted[2] == 32767.0f / 32768.0f, "PCM16 positive full scale uses 2^15 denominator");
    juce::AudioData::ConverterInstance<Int32Source, Dest> int32Converter(2, 1);
    int32Converter.convertSamples(converted, 0, pcm24in32, 0, 3);
    expect(converted[0] == -1.0f && converted[1] == 0.5f, "left-aligned PCM24-in32 full scale is preserved");
    expect(converted[2] == 8388607.0f / 8388608.0f, "24-bit positive full scale is not divided by an extra 256");
    const float source[] { -1.0f, 0.25f, 0.5f, -0.5f, 1.0f, -0.25f };
    juce::AudioData::ConverterInstance<FloatSource, Dest> floatConverter(2, 1);
    floatConverter.convertSamples(converted, 0, source, 0, 3);
    expect(converted[0] == -1.0f && converted[1] == 0.5f && converted[2] == 1.0f,
           "float WASAPI channel 1 is copied without channel-count gain");
    floatConverter.convertSamples(converted, 0, source, 1, 3);
    expect(converted[0] == 0.25f && converted[1] == -0.5f && converted[2] == -0.25f,
           "float WASAPI channel 2 is independently copied");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiser;
    testNoDefaultOpen();
    testCallbackDetachAttachRepreparesActiveDevice();
    testPcmFullScaleConversion();
    expect(mic_daw::formatPeakAndRms(1.0f, std::sqrt(0.5f)) == "PK 0.0 dBFS\nRMS -3.0 dBFS",
           "sample full-scale peak and sine RMS have distinct readouts");
    expect(mic_daw::formatPeakAndRms(0.0f, 0.0f) == juce::String(u8"PK −∞ dBFS\nRMS −∞ dBFS"),
           "silence readout is finite-safe and Unicode-safe");
    std::cout << checks << " checks; " << failures << " failures\n";
    std::cout << "No real devices, settings, audio playback, or capture were used.\n";
    return failures == 0 ? 0 : 1;
}
