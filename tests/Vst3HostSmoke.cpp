#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

int runVst3ChainStability(int argc, char* argv[]);
int runSettingsReadProbe(int argc, char* argv[]);
#if MIC_VST_BRIDGE_LOCAL_SAVED_PROBES
int runSavedChainRestoreProbe(int argc, char* argv[]);
int runSavedGainProbe(int argc, char* argv[]);
#endif

namespace
{
constexpr double smokeSampleRate = 48000.0;
constexpr int smokeBlockSize = 512;

int fail(int exitCode, const juce::String& message)
{
    std::cerr << "ERROR: " << message.toStdString() << '\n';
    return exitCode;
}

juce::File resolveModulePath(const juce::String& argument)
{
    if (juce::File::isAbsolutePath(argument))
        return juce::File(argument);

    return juce::File::getCurrentWorkingDirectory().getChildFile(argument);
}

juce::Result configureEffectLayout(juce::AudioPluginInstance& plugin)
{
    static_cast<void>(plugin.disableNonMainBuses());

    constexpr std::array<std::pair<int, int>, 4> preferredLayouts {{
        { 2, 2 }, { 1, 2 }, { 1, 1 }, { 2, 1 }
    }};

    for (const auto [candidateInputs, candidateOutputs] : preferredLayouts)
    {
        auto layout = plugin.getBusesLayout();
        if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
            continue;

        for (int bus = 0; bus < layout.inputBuses.size(); ++bus)
            layout.inputBuses.set(bus,
                                  bus == 0
                                      ? juce::AudioChannelSet::canonicalChannelSet(candidateInputs)
                                      : juce::AudioChannelSet::disabled());

        for (int bus = 0; bus < layout.outputBuses.size(); ++bus)
            layout.outputBuses.set(bus,
                                   bus == 0
                                       ? juce::AudioChannelSet::canonicalChannelSet(candidateOutputs)
                                       : juce::AudioChannelSet::disabled());

        if (plugin.setBusesLayout(layout)
            && plugin.getTotalNumInputChannels() == candidateInputs
            && plugin.getTotalNumOutputChannels() == candidateOutputs)
            return juce::Result::ok();
    }

    return juce::Result::fail("No supported mono/stereo effect layout was found");
}

bool allSamplesAreFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (!std::isfinite(buffer.getSample(channel, sample)))
                return false;

    return true;
}

class ReleaseGuard
{
public:
    explicit ReleaseGuard(juce::AudioPluginInstance& pluginToUse)
        : plugin(pluginToUse)
    {
    }

    ~ReleaseGuard()
    {
        if (!armed)
            return;

        try
        {
            plugin.releaseResources();
        }
        catch (...)
        {
        }
    }

    void arm() noexcept    { armed = true; }
    void disarm() noexcept { armed = false; }

private:
    juce::AudioPluginInstance& plugin;
    bool armed = false;
};
} // namespace

int main(int argc, char* argv[])
{
    if (argc >= 2 && std::string_view(argv[1]) == "--stability")
        return runVst3ChainStability(argc, argv);
    if (argc >= 2 && std::string_view(argv[1]) == "--inspect-settings")
        return runSettingsReadProbe(argc, argv);
#if MIC_VST_BRIDGE_LOCAL_SAVED_PROBES
    if (argc >= 2 && std::string_view(argv[1]) == "--restore-saved-chain")
        return runSavedChainRestoreProbe(argc, argv);
    if (argc >= 2 && std::string_view(argv[1]) == "--inspect-saved-gains")
        return runSavedGainProbe(argc, argv);
#endif

    if (argc != 2)
    {
        std::cerr << "Usage: Vst3HostSmoke.exe <absolute-or-relative-plugin.vst3>\n";
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    juce::String stage = "initialisation";

    try
    {
        const auto module = resolveModulePath(juce::String::fromUTF8(argv[1]));
        if (!module.exists())
            return fail(3, "VST3 module does not exist: " + module.getFullPathName());

        juce::AudioPluginFormatManager formatManager;
        juce::addDefaultFormatsToManager(formatManager);

        juce::AudioPluginFormat* vst3Format = nullptr;
        for (auto* format : formatManager.getFormats())
        {
            if (format != nullptr && format->getName() == "VST3")
            {
                vst3Format = format;
                break;
            }
        }

        if (vst3Format == nullptr)
            return fail(4, "JUCE VST3 hosting support is unavailable in this build");

        if (!vst3Format->fileMightContainThisPluginType(module.getFullPathName()))
            return fail(4, "Path is not recognised as a VST3 module: " + module.getFullPathName());

        stage = "VST3 discovery";
        juce::OwnedArray<juce::PluginDescription> discovered;
        vst3Format->findAllTypesForFile(discovered, module.getFullPathName());
        if (discovered.isEmpty())
            return fail(4, "No plugin classes were discovered in: " + module.getFullPathName());

        const auto& description = *discovered.getFirst();
        std::cout << "Discovered " << discovered.size() << " class(es); testing: "
                  << description.name.toStdString() << '\n';

        stage = "plugin instantiation";
        juce::String creationError;
        auto plugin = formatManager.createPluginInstance(description,
                                                         smokeSampleRate,
                                                         smokeBlockSize,
                                                         creationError);
        if (plugin == nullptr)
            return fail(5, "Instantiation failed: " + creationError);

        stage = "bus layout negotiation";
        const auto layoutResult = configureEffectLayout(*plugin);
        if (layoutResult.failed())
            return fail(6, layoutResult.getErrorMessage());

        const auto inputs = plugin->getTotalNumInputChannels();
        const auto outputs = plugin->getTotalNumOutputChannels();
        std::cout << "Layout: " << inputs << " input channel(s), "
                  << outputs << " output channel(s)\n";

        ReleaseGuard releaseGuard(*plugin);
        releaseGuard.arm();

        stage = "prepareToPlay";
        plugin->setRateAndBufferSizeDetails(smokeSampleRate, smokeBlockSize);
        plugin->prepareToPlay(smokeSampleRate, smokeBlockSize);

        juce::AudioBuffer<float> audio(std::max(inputs, outputs), smokeBlockSize);
        juce::MidiBuffer midi;

        stage = "processBlock";
        audio.clear();
        plugin->processBlock(audio, midi);
        if (!allSamplesAreFinite(audio))
            return fail(7, "processBlock produced a non-finite sample");

        stage = "processBlockBypassed";
        audio.clear();
        midi.clear();
        plugin->processBlockBypassed(audio, midi);
        if (!allSamplesAreFinite(audio))
            return fail(7, "processBlockBypassed produced a non-finite sample");

        stage = "state get/set round-trip";
        juce::MemoryBlock stateBefore;
        plugin->getStateInformation(stateBefore);
        if (stateBefore.getSize() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return fail(8, "Plugin state is too large for setStateInformation");

        plugin->setStateInformation(stateBefore.getData(), static_cast<int>(stateBefore.getSize()));

        juce::MemoryBlock stateAfter;
        plugin->getStateInformation(stateAfter);

        stage = "releaseResources";
        plugin->releaseResources();
        releaseGuard.disarm();

        std::cout << "State bytes: " << stateBefore.getSize()
                  << " before, " << stateAfter.getSize() << " after\n";
        std::cout << "PASS: scan, instantiate, layout, prepare, process, bypass, state, release\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        return fail(9, stage + " threw std::exception: " + exception.what());
    }
    catch (...)
    {
        return fail(9, stage + " threw an unknown C++ exception");
    }
}
