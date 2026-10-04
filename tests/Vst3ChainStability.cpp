#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// This optional test loads separate plugin instances and generates its own
// signal. It never opens an audio device, edits user settings, or records audio.
// It exercises JUCE hosting, not AudioEngine's private PluginRuntime or WASAPI.
namespace
{
struct Configuration
{
    double sampleRate;
    int maximumBlockSize;
};

constexpr std::array<Configuration, 6> configurations {{
    { 48000.0, 480 }, { 48000.0, 512 }, { 44100.0, 480 },
    { 96000.0, 960 }, { 48000.0, 128 }, { 48000.0, 1024 }
}};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int parseCount(const char* argument, int minimum, int maximum)
{
    const std::string_view text(argument);
    int result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
                && result >= minimum && result <= maximum,
            "Count outside the permitted range: " + std::string(text));
    return result;
}

void configureLayout(juce::AudioPluginInstance& plugin)
{
    static_cast<void>(plugin.disableNonMainBuses());
    constexpr std::array<std::pair<int, int>, 4> layouts {{
        { 2, 2 }, { 1, 2 }, { 1, 1 }, { 2, 1 }
    }};
    for (const auto [inputs, outputs] : layouts)
    {
        auto layout = plugin.getBusesLayout();
        if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
            continue;
        for (int bus = 0; bus < layout.inputBuses.size(); ++bus)
            layout.inputBuses.set(bus, bus == 0
                ? juce::AudioChannelSet::canonicalChannelSet(inputs)
                : juce::AudioChannelSet::disabled());
        for (int bus = 0; bus < layout.outputBuses.size(); ++bus)
            layout.outputBuses.set(bus, bus == 0
                ? juce::AudioChannelSet::canonicalChannelSet(outputs)
                : juce::AudioChannelSet::disabled());
        if (plugin.setBusesLayout(layout)
            && plugin.getTotalNumInputChannels() == inputs
            && plugin.getTotalNumOutputChannels() == outputs)
            return;
    }
    throw std::runtime_error("No mono/stereo layout for " + plugin.getName().toStdString());
}

struct Entry
{
    juce::PluginDescription description;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::MemoryBlock state;
    std::vector<std::pair<int, float>> parameters;
    bool prepared = false;
    int inputs = 0;
    int outputs = 0;

    ~Entry()
    {
        if (prepared && plugin != nullptr)
        {
            try { plugin->releaseResources(); }
            catch (...) {}
        }
    }
};

void pumpMessages(int milliseconds)
{
    static_assert(JUCE_MODAL_LOOPS_PERMITTED,
                  "The test must dispatch plugin main-thread messages while processing");
    static_cast<void>(juce::MessageManager::getInstance()->runDispatchLoopUntil(milliseconds));
}

void captureState(Entry& entry)
{
    const juce::ScopedLock lock(entry.plugin->getCallbackLock());
    entry.plugin->getStateInformation(entry.state);
    require(entry.state.getSize() > 0 && entry.state.getSize() <= 64 * 1024 * 1024,
            "Empty or excessive state for " + entry.description.name.toStdString());
    entry.parameters.clear();
    const auto& parameters = entry.plugin->getParameters();
    for (int index = 0; index < parameters.size(); ++index)
        if (parameters[index]->isAutomatable())
            entry.parameters.emplace_back(index, parameters[index]->getValue());
}

void verifyRecallParameter(const Entry& entry, std::pair<int, float> control)
{
    const auto& parameters = entry.plugin->getParameters();
    const auto [index, expected] = control;
    require(juce::isPositiveAndBelow(index, parameters.size()),
            "Manual control disappeared after state restoration");
    const auto actual = parameters[index]->getValue();
    require(std::isfinite(actual) && std::abs(expected - actual) <= 0.0001f,
            entry.description.name.toStdString() + ": manual control did not restore: "
                + parameters[index]->getName(80).toStdString()
                + "; expected=" + std::to_string(expected) + "; actual=" + std::to_string(actual));
}

std::pair<int, float> perturbRecallParameter(Entry& entry, bool reportControl)
{
    require(!entry.parameters.empty(), "No automatable parameter available for a recall test");
    const auto& parameters = entry.plugin->getParameters();
    // Do not treat adaptive or derived per-band values as preset
    // invariants. Select a named manual control and test it while DSP is stopped.
    const juce::StringArray preferredNames {
        "Reduction", "Output Gain", "Gain", "Release", "Mix", "Voice", "Threshold"
    };
    for (const auto& preferred : preferredNames)
    {
        for (const auto control : entry.parameters)
        {
            const auto [index, expected] = control;
            auto* parameter = parameters[index];
            const auto name = parameter->getName(120);
            if (!name.containsIgnoreCase(preferred) || name.containsIgnoreCase("Auto")
                || name.containsIgnoreCase("Meter") || name.containsIgnoreCase("Gain Reduction")
                || (preferred == "Threshold" && !name.equalsIgnoreCase("Threshold"))
                || parameter->isMetaParameter())
                continue;
            parameter->setValueNotifyingHost(expected < 0.5f ? 0.8f : 0.2f);
            pumpMessages(10);
            require(std::abs(parameter->getValue() - expected) > 0.001f,
                    "Manual control could not be changed for the recall test: "
                        + entry.description.name.toStdString() + ": " + name.toStdString());
            if (reportControl)
                std::cout << "RECALL CONTROL: " << entry.description.name << ": " << name << '\n';
            return control;
        }
    }
    for (const auto [index, value] : entry.parameters)
        std::cout << "AVAILABLE CONTROL: " << entry.description.name << ": "
                  << parameters[index]->getName(120) << "=" << value << '\n';
    throw std::runtime_error("No recognised manual control for " + entry.description.name.toStdString()
                             + "; enumerate its controls before extending this test");
}

void createInstance(Entry& entry, juce::AudioPluginFormatManager& manager,
                    Configuration configuration)
{
    entry.plugin.reset();
    juce::String error;
    entry.plugin = manager.createPluginInstance(entry.description,
        configuration.sampleRate, configuration.maximumBlockSize, error);
    require(entry.plugin != nullptr, "Instantiation: " + error.toStdString());
    configureLayout(*entry.plugin);
    entry.inputs = entry.plugin->getTotalNumInputChannels();
    entry.outputs = entry.plugin->getTotalNumOutputChannels();
}

struct Metrics
{
    std::uint64_t frames = 0;
    std::uint64_t bypassCalls = 0;
    std::uint64_t callsOverNominalBlockTime = 0;
    double elapsedProcessingMs = 0.0;
    double maximumProcessingMs = 0.0;
    double audioSeconds = 0.0;
    float peak = 0.0f;
};

Metrics processCycle(const std::vector<std::unique_ptr<Entry>>& entries,
                     Configuration configuration, int blockCount, int cycle)
{
    Metrics metrics;
    juce::AudioBuffer<float> storage(2, configuration.maximumBlockSize);
    juce::MidiBuffer midi;
    std::uint32_t random = 0x1257ab31u + static_cast<std::uint32_t>(cycle);
    std::uint64_t position = 0;

    for (int block = 0; block < blockCount; ++block)
    {
        // Regular device-size blocks plus valid short tail/boundary blocks.
        const auto tail = block % 67;
        const auto samples = tail == 1 ? 1 : tail == 2 ? configuration.maximumBlockSize - 1
            : tail == 3 ? configuration.maximumBlockSize / 2 : configuration.maximumBlockSize;
        juce::AudioBuffer<float> audio(storage.getArrayOfWritePointers(), 2, samples);
        const auto silent = block % 200 < 16;
        for (int sample = 0; sample < samples; ++sample, ++position)
        {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            const auto noise = (static_cast<float>(random & 0xffffu) / 65535.0f - 0.5f) * 0.03f;
            const auto time = static_cast<double>(position) / configuration.sampleRate;
            const auto tone = static_cast<float>(0.1 * std::sin(2.0 * juce::MathConstants<double>::pi * 173.0 * time)
                + 0.04 * std::sin(2.0 * juce::MathConstants<double>::pi * 1097.0 * time));
            const auto impulse = position % 24013 == 0 ? 0.5f : 0.0f;
            audio.setSample(0, sample, silent ? 0.0f : tone + noise + impulse);
            audio.setSample(1, sample, silent ? 0.0f : tone - noise + impulse);
        }

        double blockProcessingMs = 0.0;
        for (std::size_t slot = 0; slot < entries.size(); ++slot)
        {
            const auto& entry = *entries[slot];
            const auto bypassed = block % 97 >= 89
                && slot == static_cast<std::size_t>(cycle) % entries.size();
            const auto start = std::chrono::steady_clock::now();
            {
                const juce::ScopedLock lock(entry.plugin->getCallbackLock());
                if (entry.plugin->isSuspended())
                    audio.clear();
                else
                {
                    for (int channel = entry.inputs; channel < 2; ++channel)
                        audio.clear(channel, 0, samples);
                    juce::AudioBuffer<float> pluginBlock(audio.getArrayOfWritePointers(),
                        std::max(entry.inputs, entry.outputs), samples);
                    midi.clear();
                    if (bypassed)
                    {
                        entry.plugin->processBlockBypassed(pluginBlock, midi);
                        ++metrics.bypassCalls;
                    }
                    else
                        entry.plugin->processBlock(pluginBlock, midi);
                    if (entry.outputs == 1 || (bypassed && entry.inputs == 1))
                        audio.copyFrom(1, 0, audio, 0, 0, samples);
                }
            }
            blockProcessingMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < samples; ++sample)
                    if (!std::isfinite(audio.getSample(channel, sample)))
                        throw std::runtime_error("NaN/Inf output from " + entry.description.name.toStdString());
        }
        for (int channel = 0; channel < 2; ++channel)
            metrics.peak = std::max(metrics.peak, audio.getMagnitude(channel, 0, samples));
        metrics.frames += static_cast<std::uint64_t>(samples);
        metrics.elapsedProcessingMs += blockProcessingMs;
        metrics.maximumProcessingMs = std::max(metrics.maximumProcessingMs, blockProcessingMs);
        if (blockProcessingMs > 1000.0 * samples / configuration.sampleRate)
            ++metrics.callsOverNominalBlockTime;
        if (block % 32 == 0)
            std::this_thread::yield();
    }
    metrics.audioSeconds = static_cast<double>(metrics.frames) / configuration.sampleRate;
    return metrics;
}
} // namespace

int runVst3ChainStability(int argc, char* argv[])
{
    if (argc < 5 || argc > 12)
    {
        std::cerr << "Usage: Vst3HostSmoke.exe --stability <cycles:1..100> "
                     "<blocks-per-cycle:32..20000> <plugin.vst3> [up to 8 plugins]\n";
        return 2;
    }
    std::cout << std::unitbuf;
    juce::ScopedJuceInitialiser_GUI initialiser;
    try
    {
        const auto cycles = parseCount(argv[2], 1, 100);
        const auto blocks = parseCount(argv[3], 32, 20000);
        juce::AudioPluginFormatManager manager;
        juce::addDefaultFormatsToManager(manager);
        juce::AudioPluginFormat* format = nullptr;
        for (auto* candidate : manager.getFormats())
            if (candidate->getName() == "VST3")
                format = candidate;
        require(format != nullptr, "No VST3 host format");
        std::vector<std::unique_ptr<Entry>> entries;
        for (int index = 4; index < argc; ++index)
        {
            const auto argument = juce::String::fromUTF8(argv[index]);
            const auto module = juce::File::isAbsolutePath(argument)
                ? juce::File(argument) : juce::File::getCurrentWorkingDirectory().getChildFile(argument);
            require(module.exists(), "Missing module: " + module.getFullPathName().toStdString());
            juce::OwnedArray<juce::PluginDescription> descriptions;
            format->findAllTypesForFile(descriptions, module.getFullPathName());
            require(!descriptions.isEmpty(), "No VST3 classes discovered");
            auto entry = std::make_unique<Entry>();
            entry->description = *descriptions.getFirst();
            createInstance(*entry, manager, configurations.front());
            pumpMessages(1);
            captureState(*entry);
            std::cout << "SLOT " << entries.size() + 1 << ": "
                      << entry->description.name << "; layout=" << entry->inputs << "/"
                      << entry->outputs << "; initial state bytes=" << entry->state.getSize() << '\n';
            entries.push_back(std::move(entry));
        }
        std::cout << "No devices/output/recording. Factory/current plugin defaults; "
                     "the running application's presets are NOT read.\n";

        Metrics total;
        std::uint64_t recreatedInstances = 0;
        std::uint64_t recallChecks = 0;
        for (int cycle = 0; cycle < cycles; ++cycle)
        {
            const auto configuration = configurations[static_cast<std::size_t>(cycle) % configurations.size()];
            const auto recreate = cycle > 0 && cycle % 4 == 0;
            for (auto& entry : entries)
            {
                if (recreate)
                {
                    createInstance(*entry, manager, configuration);
                    ++recreatedInstances;
                }
                const auto recallControl = perturbRecallParameter(*entry, cycle == 0);
                entry->plugin->setStateInformation(entry->state.getData(), static_cast<int>(entry->state.getSize()));
                pumpMessages(10);
                require(entry->plugin->getTotalNumInputChannels() == entry->inputs
                            && entry->plugin->getTotalNumOutputChannels() == entry->outputs,
                        "State restore changed the main bus layout");
                verifyRecallParameter(*entry, recallControl);
                ++recallChecks;
                entry->prepared = true;
                entry->plugin->setRateAndBufferSizeDetails(configuration.sampleRate, configuration.maximumBlockSize);
                entry->plugin->prepareToPlay(configuration.sampleRate, configuration.maximumBlockSize);
            }
            pumpMessages(1);

            Metrics result;
            std::exception_ptr failure;
            std::atomic<bool> done { false };
            std::thread worker([&]
            {
                try { result = processCycle(entries, configuration, blocks, cycle); }
                catch (...) { failure = std::current_exception(); }
                done.store(true, std::memory_order_release);
            });
            while (!done.load(std::memory_order_acquire))
                pumpMessages(5);
            worker.join();
            if (failure != nullptr)
                std::rethrow_exception(failure);

            for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            {
                (*it)->plugin->releaseResources();
                (*it)->prepared = false;
            }
            for (auto& entry : entries)
            {
                // Plugins may legitimately publish new adaptive parameter values
                // while processing; only finite audio is required after DSP.
                captureState(*entry);
            }
            total.frames += result.frames;
            total.audioSeconds += result.audioSeconds;
            total.bypassCalls += result.bypassCalls;
            total.elapsedProcessingMs += result.elapsedProcessingMs;
            total.callsOverNominalBlockTime += result.callsOverNominalBlockTime;
            total.maximumProcessingMs = std::max(total.maximumProcessingMs, result.maximumProcessingMs);
            total.peak = std::max(total.peak, result.peak);
            std::cout << "CYCLE " << cycle + 1 << "/" << cycles
                      << ": rate=" << configuration.sampleRate << "; maxBlock=" << configuration.maximumBlockSize
                      << "; audioSeconds=" << result.audioSeconds << "; peak=" << result.peak
                      << "; maxProcessMs=" << result.maximumProcessingMs << "; recreate=" << recreate << '\n';
        }
        std::cout << "PASS: plugins=" << entries.size() << "; cycles=" << cycles
                  << "; chainBlocks=" << static_cast<std::uint64_t>(cycles) * static_cast<std::uint64_t>(blocks)
                  << "; frames=" << total.frames << "; audioSeconds=" << total.audioSeconds
                  << "; bypassCalls=" << total.bypassCalls << "; peak=" << total.peak
                  << "; parameterRecallChecks=" << recallChecks
                  << "; recreatedInstances=" << recreatedInstances << '\n';
        std::cout << "TIMING (offline, not a live XRun measurement): DSP ms=" << total.elapsedProcessingMs
                  << "; max block ms=" << total.maximumProcessingMs
                  << "; calls over nominal block duration=" << total.callsOverNominalBlockTime << '\n';
        std::cout << "Verified finite output, prepare/release, and state/parameter recall after "
                     "a named manual control change. Bypass/recreation coverage is counted above; "
                     "the user's live presets and AudioEngine are not tested.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 9;
    }
    catch (...)
    {
        std::cerr << "FAIL: unknown C++ exception\n";
        return 9;
    }
}
