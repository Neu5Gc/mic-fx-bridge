#include "AudioEngine.h"
#include "StreamDiagnostics.h"
#include "DeviceManagerSetup.h"
#include "InputChannelRouting.h"
#include "PluginStatePersistence.h"
#include "UiText.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
constexpr auto kDeviceScanIntervalMs = 1000;
constexpr auto kPreferredSampleRate = 48000.0;
constexpr auto kFallbackBlockSize = 512;

juce::String endpointStateName(AudioEngine::EndpointState state)
{
    switch (state)
    {
        case AudioEngine::EndpointState::stopped:    return mic_daw::uiText(u8"중지됨", "Stopped");
        case AudioEngine::EndpointState::waiting:    return mic_daw::uiText(u8"장치 대기 중", "Waiting for device");
        case AudioEngine::EndpointState::recovering: return mic_daw::uiText(u8"복구 중", "Recovering");
        case AudioEngine::EndpointState::running:    return mic_daw::uiText(u8"정상", "Running");
        case AudioEngine::EndpointState::error:      return mic_daw::uiText(u8"오류", "Error");
    }

    return mic_daw::uiText(u8"알 수 없음", "Unknown");
}

float peakOf(const float* data, int numSamples) noexcept
{
    if (data == nullptr || numSamples <= 0)
        return 0.0f;

    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        if (std::isfinite(data[i]))
            peak = std::max(peak, std::abs(data[i]));

    return peak;
}
} // namespace

//==============================================================================
void AudioEngine::SharedWasapiDeviceManager::createAudioDeviceTypes(
    juce::OwnedArray<juce::AudioIODeviceType>& types)
{
#if JUCE_WINDOWS
    if (auto* wasapi = juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(
            juce::WASAPIDeviceMode::shared))
        types.add(wasapi);
#else
    juce::AudioDeviceManager::createAudioDeviceTypes(types);
#endif
}

//==============================================================================
AudioEngine::StereoRingBuffer::StereoRingBuffer(std::size_t requestedCapacity)
    : capacityFrames(std::max<std::size_t>(requestedCapacity, 2))
{
    channels[0].resize(capacityFrames, 0.0f);
    channels[1].resize(capacityFrames, 0.0f);
}

int AudioEngine::StereoRingBuffer::push(const float* left,
                                        const float* right,
                                        int frames) noexcept
{
    if (left == nullptr || right == nullptr || frames <= 0)
        return 0;

    const auto read = readPosition.load(std::memory_order_acquire);
    const auto write = writePosition.load(std::memory_order_relaxed);
    const auto occupied = static_cast<std::size_t>(std::min<std::uint64_t>(
        write >= read ? write - read : 0,
        capacityFrames));
    const auto freeFrames = capacityFrames - occupied;
    const auto framesToWrite = std::min<std::size_t>(static_cast<std::size_t>(frames), freeFrames);

    for (std::size_t i = 0; i < framesToWrite; ++i)
    {
        const auto index = static_cast<std::size_t>((write + i) % capacityFrames);
        channels[0][index] = left[i];
        channels[1][index] = right[i];
    }

    writePosition.store(write + framesToWrite, std::memory_order_release);

    if (framesToWrite < static_cast<std::size_t>(frames))
    {
        overruns.fetch_add(static_cast<std::uint64_t>(frames) - framesToWrite,
                           std::memory_order_relaxed);
        clearRequested.store(true, std::memory_order_release);
    }

    return static_cast<int>(framesToWrite);
}

std::size_t AudioEngine::StereoRingBuffer::available() const noexcept
{
    const auto read = readPosition.load(std::memory_order_relaxed);
    const auto write = writePosition.load(std::memory_order_acquire);
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        write >= read ? write - read : 0,
        capacityFrames));
}

float AudioEngine::StereoRingBuffer::peek(int channel, std::size_t offset) const noexcept
{
    const auto read = readPosition.load(std::memory_order_relaxed);
    const auto channelIndex = static_cast<std::size_t>(juce::jlimit(0, 1, channel));
    return channels[channelIndex][static_cast<std::size_t>((read + offset) % capacityFrames)];
}

void AudioEngine::StereoRingBuffer::consume(std::size_t frames) noexcept
{
    const auto readable = available();
    const auto amount = std::min(frames, readable);
    readPosition.fetch_add(amount, std::memory_order_release);
}

void AudioEngine::StereoRingBuffer::requestClear() noexcept
{
    clearRequested.store(true, std::memory_order_release);
}

bool AudioEngine::StereoRingBuffer::applyPendingClear() noexcept
{
    if (!clearRequested.exchange(false, std::memory_order_acq_rel))
        return false;

    readPosition.store(writePosition.load(std::memory_order_acquire), std::memory_order_release);
    return true;
}

std::uint64_t AudioEngine::StereoRingBuffer::getOverrunCount() const noexcept
{
    return overruns.load(std::memory_order_relaxed);
}

//==============================================================================
void AudioEngine::InputCallback::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData,
    int numInputChannels,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    if (outputChannelData != nullptr)
        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

    owner.inputEndpoint.lastCallbackMs.store(AudioEngine::nowMs(), std::memory_order_relaxed);

    if (!owner.inputEndpoint.callbackIsAuthorized())
    {
        rmsMeter.reset();
        owner.inputPeak.reset();
        owner.inputRms.store(0.0f, std::memory_order_relaxed);
        return;
    }

    if (numInputChannels <= 0 || inputChannelData == nullptr || numSamples <= 0)
    {
        rmsMeter.reset();
        owner.inputPeak.reset();
        owner.inputRms.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const auto mode = owner.inputChannelMode.load(std::memory_order_relaxed);
    const auto selected = mic_daw::selectInputChannels(inputChannelData, numInputChannels, mode);
    if (!selected.hasAudioData())
    {
        rmsMeter.reset();
        owner.inputPeak.reset();
        owner.inputRms.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const auto* left = selected.left;
    const auto* right = selected.right;

    if (meterChannelMode != mode)
    {
        rmsMeter.reset();
        owner.inputPeak.reset();
        meterChannelMode = mode;
    }

    const float* selectedChannels[] { left, right };
    rmsMeter.process(selectedChannels, 2, numSamples);
    owner.inputRms.store(rmsMeter.level(), std::memory_order_relaxed);
    owner.inputPeak.publish(std::max(peakOf(left, numSamples), peakOf(right, numSamples)));
    owner.ring.push(left, right, numSamples);
}

void AudioEngine::InputCallback::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    const auto sampleRate = device->getCurrentSampleRate();
    const auto blockSize = std::max(32, device->getCurrentBufferSizeSamples());

    rmsMeter.prepare(sampleRate);
    meterChannelMode = -1;
    owner.inputPeak.reset();
    owner.inputRms.store(0.0f, std::memory_order_relaxed);
    owner.inputEndpoint.setCallbackDevice(device->getName());
    owner.inputEndpoint.sampleRate.store(sampleRate, std::memory_order_relaxed);
    owner.inputEndpoint.bufferSize.store(blockSize, std::memory_order_relaxed);
    owner.inputEndpoint.lastCallbackMs.store(AudioEngine::nowMs(), std::memory_order_relaxed);
    owner.inputEndpoint.callbackError.store(false, std::memory_order_relaxed);

    owner.inputEndpoint.callbackRunning.store(true, std::memory_order_release);
    owner.ring.requestClear();
}

void AudioEngine::InputCallback::audioDeviceStopped()
{
    owner.inputEndpoint.clearCallbackDevice();
    owner.inputEndpoint.callbackRunning.store(false, std::memory_order_release);
    owner.inputPeak.reset();
    owner.inputRms.store(0.0f, std::memory_order_relaxed);
    owner.ring.requestClear();
}

void AudioEngine::InputCallback::audioDeviceError(const juce::String&)
{
    owner.inputEndpoint.clearCallbackDevice();
    owner.inputEndpoint.callbackError.store(true, std::memory_order_release);
    owner.inputPeak.reset();
    owner.inputRms.store(0.0f, std::memory_order_relaxed);
    owner.ring.requestClear();
}

//==============================================================================
void AudioEngine::OutputCallback::audioDeviceIOCallbackWithContext(
    const float* const*,
    int,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    owner.outputEndpoint.lastCallbackMs.store(AudioEngine::nowMs(), std::memory_order_relaxed);

    if (outputChannelData != nullptr)
        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

    if (!owner.outputEndpoint.callbackIsAuthorized())
    {
        rmsMeter.reset();
        owner.outputPeak.reset();
        owner.outputRms.store(0.0f, std::memory_order_relaxed);
        return;
    }

    if (numOutputChannels <= 0 || outputChannelData == nullptr || numSamples <= 0)
    {
        rmsMeter.reset();
        owner.outputPeak.reset();
        owner.outputRms.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const auto blockCapacity = processingBuffer.getNumSamples();
    if (blockCapacity <= 0)
    {
        rmsMeter.process(outputChannelData, numOutputChannels, numSamples);
        owner.outputPeak.publish(0.0f);
        owner.outputRms.store(rmsMeter.level(), std::memory_order_relaxed);
        return;
    }

    float peak = 0.0f;
    for (int offset = 0; offset < numSamples; offset += blockCapacity)
    {
        const auto block = std::min(blockCapacity, numSamples - offset);
        juce::AudioBuffer<float> blockBuffer(processingBuffer.getArrayOfWritePointers(),
                                             processingBuffer.getNumChannels(),
                                             block);
        blockBuffer.clear();
        owner.renderInput(blockBuffer, block);
        owner.processOutput(blockBuffer, block);

        for (int channel = 0; channel < std::min(2, numOutputChannels); ++channel)
        {
            if (outputChannelData[channel] == nullptr)
                continue;

            const auto sourceChannel = std::min(channel, blockBuffer.getNumChannels() - 1);
            juce::FloatVectorOperations::copy(outputChannelData[channel] + offset,
                                              blockBuffer.getReadPointer(sourceChannel),
                                              block);
            peak = std::max(peak, peakOf(blockBuffer.getReadPointer(sourceChannel), block));
        }
    }

    // Measure the buffers actually handed to WASAPI, after the entire chain,
    // recovery fade, underrun padding and copies (including any silent extra
    // active device channels). This does not touch the emitted samples.
    rmsMeter.process(outputChannelData, numOutputChannels, numSamples);
    owner.outputRms.store(rmsMeter.level(), std::memory_order_relaxed);
    owner.outputPeak.publish(peak);
}

void AudioEngine::OutputCallback::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    rmsMeter.prepare(device->getCurrentSampleRate());
    owner.outputPeak.reset();
    owner.outputRms.store(0.0f, std::memory_order_relaxed);
    owner.outputEndpoint.setCallbackDevice(device->getName());
    const auto blockSize = std::max(32, device->getCurrentBufferSizeSamples());
    processingBuffer.setSize(2, blockSize, false, true, false);
    processingBuffer.clear();

    owner.outputEndpoint.sampleRate.store(device->getCurrentSampleRate(), std::memory_order_relaxed);
    owner.outputEndpoint.bufferSize.store(blockSize, std::memory_order_relaxed);
    owner.outputEndpoint.lastCallbackMs.store(AudioEngine::nowMs(), std::memory_order_relaxed);
    owner.outputEndpoint.callbackError.store(false, std::memory_order_relaxed);
    owner.outputEndpoint.callbackRunning.store(true, std::memory_order_release);
    owner.resamplePosition = 0.0;
    owner.inputPrimed = false;
    owner.inputFadeGain = 0.0f;
    owner.ring.requestClear();
    owner.preparePluginChain(device->getCurrentSampleRate(), blockSize);
}

void AudioEngine::OutputCallback::audioDeviceStopped()
{
    owner.outputEndpoint.clearCallbackDevice();
    owner.outputEndpoint.callbackRunning.store(false, std::memory_order_release);
    owner.outputPeak.reset();
    owner.outputRms.store(0.0f, std::memory_order_relaxed);
    owner.releasePluginChain();
}

void AudioEngine::OutputCallback::audioDeviceError(const juce::String&)
{
    owner.outputEndpoint.clearCallbackDevice();
    owner.outputEndpoint.callbackError.store(true, std::memory_order_release);
    owner.outputPeak.reset();
    owner.outputRms.store(0.0f, std::memory_order_relaxed);
}

//==============================================================================
juce::Result AudioEngine::PluginRuntime::install(
    std::unique_ptr<juce::AudioPluginInstance> newPlugin)
{
    if (newPlugin == nullptr)
        return juce::Result::fail(
            mic_daw::uiText(u8"플러그인 인스턴스를 만들지 못했습니다.",
                            "Could not create the plugin instance."));

    static_cast<void>(newPlugin->disableNonMainBuses());

    constexpr std::array<std::pair<int, int>, 4> preferredLayouts {{
        { 2, 2 }, { 1, 2 }, { 1, 1 }, { 2, 1 }
    }};

    bool layoutConfigured = false;
    for (const auto [candidateInputs, candidateOutputs] : preferredLayouts)
    {
        auto layout = newPlugin->getBusesLayout();
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

        if (newPlugin->setBusesLayout(layout)
            && newPlugin->getTotalNumInputChannels() == candidateInputs
            && newPlugin->getTotalNumOutputChannels() == candidateOutputs)
        {
            layoutConfigured = true;
            break;
        }
    }

    if (!layoutConfigured)
        return juce::Result::fail(
            mic_daw::uiText(u8"이 플러그인이 지원하는 mono/stereo 효과 입출력을 협상하지 못했습니다.",
                            "Could not negotiate a supported mono/stereo effect layout with this plugin."));

    const auto inputs = newPlugin->getTotalNumInputChannels();
    const auto outputs = newPlugin->getTotalNumOutputChannels();

    if (inputs < 1 || inputs > 2 || outputs < 1 || outputs > 2)
        return juce::Result::fail(
            mic_daw::uiText(u8"현재 버전은 mono/stereo 오디오 효과만 지원합니다.",
                            "This version supports mono/stereo audio effects only."));

    clear();
    inputChannels = inputs;
    outputChannels = outputs;
    plugin = std::move(newPlugin);
    parameterAffectsState.reserve(static_cast<std::size_t>(plugin->getParameters().size()));
    for (const auto* parameter : plugin->getParameters())
        parameterAffectsState.push_back(static_cast<unsigned char>(
            (static_cast<int>(parameter->getCategory()) >> 16) != 2));
    plugin->addListener(this);
    faulted.store(false);
    disabledAfterProcessFault.store(false);
    lifecycleFaulted.store(false);
    return juce::Result::ok();
}

bool AudioEngine::PluginRuntime::hasExpectedLayout() const
{
    return plugin != nullptr
           && plugin->getTotalNumInputChannels() == inputChannels
           && plugin->getTotalNumOutputChannels() == outputChannels;
}

void AudioEngine::PluginRuntime::clear()
{
    if (plugin != nullptr)
        plugin->removeListener(this);
    release();
    plugin.reset();
    parameterAffectsState.clear();
    inputChannels = 2;
    outputChannels = 2;
    faulted.store(false);
    disabledAfterProcessFault.store(false);
    lifecycleFaulted.store(false);
    reportedLatencySamples.store(0, std::memory_order_relaxed);
    processReady.store(false, std::memory_order_release);
    releasePending = false;
    dryBuffer.setSize(0, 0);
}

void AudioEngine::PluginRuntime::prepare(double sampleRate, int maximumBlockSize)
{
    if (plugin == nullptr || sampleRate <= 0.0 || maximumBlockSize <= 0)
        return;

    processReady.store(false, std::memory_order_release);
    const juce::ScopedLock callbackLock(plugin->getCallbackLock());

    if (releasePending)
    {
        try
        {
            plugin->releaseResources();
            releasePending = false;
        }
        catch (...)
        {
            bypassed.store(true, std::memory_order_release);
            lifecycleFaulted.store(true, std::memory_order_release);
            faulted.store(true, std::memory_order_release);
            return;
        }
    }

    try
    {
        dryBuffer.setSize(2, maximumBlockSize, false, false, true);
        releasePending = true;
        plugin->setRateAndBufferSizeDetails(sampleRate, maximumBlockSize);
        plugin->prepareToPlay(sampleRate, maximumBlockSize);
        lifecycleFaulted.store(false, std::memory_order_release);
        processReady.store(true, std::memory_order_release);

        try
        {
            reportedLatencySamples.store(std::max(0, plugin->getLatencySamples()),
                                         std::memory_order_relaxed);
        }
        catch (...)
        {
            reportedLatencySamples.store(0, std::memory_order_relaxed);
        }
    }
    catch (...)
    {
        processReady.store(false, std::memory_order_release);
        if (releasePending)
        {
            try
            {
                plugin->releaseResources();
                releasePending = false;
            }
            catch (...)
            {
                releasePending = true;
            }
        }
        reportedLatencySamples.store(0, std::memory_order_relaxed);
        bypassed.store(true, std::memory_order_release);
        lifecycleFaulted.store(true, std::memory_order_release);
        faulted.store(true, std::memory_order_release);
    }
}

void AudioEngine::PluginRuntime::release()
{
    processReady.store(false, std::memory_order_release);

    if (plugin == nullptr)
    {
        releasePending = false;
        lifecycleFaulted.store(false, std::memory_order_release);
        return;
    }

    const juce::ScopedLock callbackLock(plugin->getCallbackLock());
    if (releasePending)
    {
        try
        {
            plugin->releaseResources();
            releasePending = false;
        }
        catch (...)
        {
            faulted.store(true, std::memory_order_release);
            lifecycleFaulted.store(true, std::memory_order_release);
            bypassed.store(true, std::memory_order_release);
            return;
        }
    }

    lifecycleFaulted.store(false, std::memory_order_release);
}

void AudioEngine::PluginRuntime::process(juce::AudioBuffer<float>& buffer,
                                         int numSamples) noexcept
{
    if (plugin == nullptr || !processReady.load(std::memory_order_acquire)
        || disabledAfterProcessFault.load(std::memory_order_acquire))
        return;

    const juce::ScopedLock callbackLock(plugin->getCallbackLock());
    if (!processReady.load(std::memory_order_acquire)
        || disabledAfterProcessFault.load(std::memory_order_acquire))
        return;

    for (int channel = 0; channel < std::min(2, buffer.getNumChannels()); ++channel)
        dryBuffer.copyFrom(channel, 0, buffer, channel, 0, numSamples);

    try
    {
        if (plugin->isSuspended())
        {
            buffer.clear(0, numSamples);
            return;
        }

        for (int channel = inputChannels; channel < buffer.getNumChannels(); ++channel)
            buffer.clear(channel, 0, numSamples);

        midi.clear();
        const auto pluginChannels = std::max(inputChannels, outputChannels);
        juce::AudioBuffer<float> pluginBlock(buffer.getArrayOfWritePointers(),
                                             pluginChannels,
                                             numSamples);
        const auto bypassing = bypassed.load(std::memory_order_relaxed);
        if (bypassing)
            plugin->processBlockBypassed(pluginBlock, midi);
        else
            plugin->processBlock(pluginBlock, midi);

        if ((outputChannels == 1 || (bypassing && inputChannels == 1))
            && buffer.getNumChannels() > 1)
            buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
    }
    catch (...)
    {
        for (int channel = 0; channel < std::min(2, buffer.getNumChannels()); ++channel)
            buffer.copyFrom(channel, 0, dryBuffer, channel, 0, numSamples);
        faulted.store(true, std::memory_order_release);
        disabledAfterProcessFault.store(true, std::memory_order_release);
        bypassed.store(true, std::memory_order_release);
    }
}

//==============================================================================
AudioEngine::AudioEngine()
{
    pluginSlots.reserve(kMaxPluginSlots);
    juce::addDefaultFormatsToManager(pluginFormats);
    initialiseDeviceManagers();
    refreshDeviceLists();
    startTimer(250);
}

AudioEngine::~AudioEngine()
{
    stopTimer();
    inputManager.removeChangeListener(this);
    outputManager.removeChangeListener(this);

    if (inputCallbackRegistered)
        inputManager.removeAudioCallback(&inputCallback);
    if (outputCallbackRegistered)
        outputManager.removeAudioCallback(&outputCallback);

    inputManager.closeAudioDevice();
    outputManager.closeAudioDevice();
    releasePluginChain();
    pluginSlots.clear();
}

void AudioEngine::initialiseDeviceManagers()
{
    const auto inputError = mic_daw::initialiseWithoutOpeningDevice(inputManager);
    const auto outputError = mic_daw::initialiseWithoutOpeningDevice(outputManager);

    inputManager.addAudioCallback(&inputCallback);
    outputManager.addAudioCallback(&outputCallback);
    inputCallbackRegistered = true;
    outputCallbackRegistered = true;
    inputManager.addChangeListener(this);
    outputManager.addChangeListener(this);

    if (inputError.isNotEmpty())
        addLog(mic_daw::uiText(u8"입력 WASAPI 초기화: ", "Input WASAPI initialization: ")
               + inputError);
    if (outputError.isNotEmpty())
        addLog(mic_daw::uiText(u8"출력 WASAPI 초기화: ", "Output WASAPI initialization: ")
               + outputError);
}

void AudioEngine::setBridgeEnabled(bool shouldRun)
{
    if (bridgeEnabled == shouldRun)
        return;

    bridgeEnabled = shouldRun;

    if (!bridgeEnabled)
    {
        inputEndpoint.disarmCallback();
        outputEndpoint.disarmCallback();
        inputEndpoint.recovery.stop();
        outputEndpoint.recovery.stop();
        inputManager.closeAudioDevice();
        outputManager.closeAudioDevice();
        ring.requestClear();
        addLog(mic_daw::uiText(u8"브리지를 중지했습니다.", "Bridge stopped."));
    }
    else
    {
        addLog(mic_daw::uiText(u8"브리지를 시작합니다.", "Starting bridge."));
        requestImmediateRetry(outputEndpoint);
        requestImmediateRetry(inputEndpoint);
    }

    notifyStateChanged();
}

void AudioEngine::setAutoRecover(bool shouldRecover)
{
    autoRecover = shouldRecover;
    inputEndpoint.recovery.setAutoRecoveryEnabled(shouldRecover);
    outputEndpoint.recovery.setAutoRecoveryEnabled(shouldRecover);

    if (autoRecover && bridgeMayUseDevices())
    {
        if (inputEndpoint.recovery.status() != mic_daw::RecoveryState::Status::running
            || !inputEndpoint.callbackRunning.load())
            requestImmediateRetry(inputEndpoint);
        if (outputEndpoint.recovery.status() != mic_daw::RecoveryState::Status::running
            || !outputEndpoint.callbackRunning.load())
            requestImmediateRetry(outputEndpoint);
    }

    addLog(autoRecover ? mic_daw::uiText(u8"자동 복구를 켰습니다.", "Automatic recovery enabled.")
                       : mic_daw::uiText(u8"자동 복구를 껐습니다.", "Automatic recovery disabled."));
    notifyStateChanged();
}

void AudioEngine::setInputDevice(const juce::String& name)
{
    if (inputEndpoint.desiredDevice == name)
        return;

    // Disable the old binding before publishing a new desired device. This
    // closes the small control/audio-thread window in which samples from the
    // previous device could otherwise pass through a newly matching profile.

    inputEndpoint.disarmCallback();
    // closeAudioDevice() synchronously drains/stops the current input callback
    // before the desired binding and FIFO generation change. Otherwise an
    // already-authorized old-device block could be published after the clear
    // request and survive into the new route.
    inputManager.closeAudioDevice();
    inputEndpoint.desiredDevice = name;
    inputEndpoint.lastError.clear();
    ring.requestClear();

    if (bridgeMayUseDevices())
        requestImmediateRetry(inputEndpoint);

    notifyStateChanged();
}

void AudioEngine::setOutputDevice(const juce::String& name)
{
    if (outputEndpoint.desiredDevice == name)
        return;

    outputEndpoint.disarmCallback();
    outputEndpoint.desiredDevice = name;
    outputEndpoint.lastError.clear();
    ring.requestClear();

    if (bridgeMayUseDevices())
        requestImmediateRetry(outputEndpoint);

    notifyStateChanged();
}

const juce::String& AudioEngine::getDesiredInputDevice() const noexcept
{
    return inputEndpoint.desiredDevice;
}

const juce::String& AudioEngine::getDesiredOutputDevice() const noexcept
{
    return outputEndpoint.desiredDevice;
}

void AudioEngine::setInputChannelMode(int mode)
{
    const auto clampedMode = juce::jlimit(0, 2, mode);
    if (inputChannelMode.load(std::memory_order_relaxed) == clampedMode)
        return;

    // Input routing is sampled by the capture callback. Stop that callback
    // before changing the route so an in-flight block from the old channel
    // cannot be corrected with the new channel's profile.

    if (bridgeMayUseDevices())
    {
        inputEndpoint.disarmCallback();
        inputManager.closeAudioDevice();
    }

    inputChannelMode.store(clampedMode, std::memory_order_release);
    ring.requestClear();

    if (bridgeMayUseDevices())
        requestImmediateRetry(inputEndpoint);

    notifyStateChanged();
}

int AudioEngine::getInputChannelMode() const noexcept
{
    return inputChannelMode.load();
}

void AudioEngine::refreshDeviceLists()
{
    if (auto* type = getWasapiType(inputManager))
    {
        type->scanForDevices();
        inputDevices = type->getDeviceNames(true);
    }
    else
    {
        inputDevices.clear();
    }

    if (auto* type = getWasapiType(outputManager))
    {
        type->scanForDevices();
        outputDevices = type->getDeviceNames(false);
    }
    else
    {
        outputDevices.clear();
    }

    inputDevices.removeEmptyStrings();
    outputDevices.removeEmptyStrings();
    deviceListDirty = false;
    nextDeviceScanMs = nowMs() + kDeviceScanIntervalMs;
}

juce::StringArray AudioEngine::getInputDevices() const
{
    return inputDevices;
}

juce::StringArray AudioEngine::getOutputDevices() const
{
    return outputDevices;
}

juce::String AudioEngine::getDefaultInputDevice() const
{
    if (auto* type = getWasapiType(const_cast<SharedWasapiDeviceManager&>(inputManager)))
    {
        const auto index = type->getDefaultDeviceIndex(true);
        if (juce::isPositiveAndBelow(index, inputDevices.size()))
            return inputDevices[index];
    }

    return inputDevices.isEmpty() ? juce::String{} : inputDevices[0];
}

juce::String AudioEngine::findPreferredCableOutput() const
{
    for (const auto& name : outputDevices)
        if (name.containsIgnoreCase("CABLE Input"))
            return name;

    for (const auto& name : outputDevices)
        if (name.containsIgnoreCase("VB-Audio") && name.containsIgnoreCase("Cable"))
            return name;

    return {};
}

void AudioEngine::retryNow()
{
    if (!bridgeMayUseDevices())
        return;

    deviceListDirty = true;
    requestImmediateRetry(outputEndpoint);
    requestImmediateRetry(inputEndpoint);
}

juce::Result AudioEngine::loadPlugin(int slotIndex, const juce::File& module)
{
    return loadPluginInternal(slotIndex, module, {}, {}, {}, false, false);
}

juce::Result AudioEngine::loadPluginInternal(int slotIndex,
                                             const juce::File& module,
                                             const juce::String& preferredIdentifier,
                                             const juce::String& preferredFormatName,
                                             const juce::String& stateBase64,
                                             bool bypassed,
                                             bool restoring)
{
    const auto slotCount = static_cast<int>(pluginSlots.size());
    if (slotIndex < 0 || slotIndex > slotCount || slotIndex >= kMaxPluginSlots)
        return juce::Result::fail(
            mic_daw::uiText(u8"유효하지 않은 FX 슬롯입니다.", "The FX slot is invalid."));

    std::unique_ptr<PluginSlot> newSlot;
    const auto created = createPluginSlot(module, preferredIdentifier, preferredFormatName,
                                          stateBase64, bypassed, newSlot);
    if (created.failed())
        return created;

    newSlot->runtime.setBypassed(bypassed);
    const auto pluginName = newSlot->name;
    auto* const publishedSlot = newSlot.get();
    const auto outputWasOpen = outputManager.getCurrentAudioDevice() != nullptr;
    std::unique_ptr<PluginSlot> replacedSlot;
    detachOutputCallback();
    if (slotIndex < slotCount)
    {
        replacedSlot = std::move(pluginSlots[static_cast<std::size_t>(slotIndex)]);
        pluginSlots[static_cast<std::size_t>(slotIndex)] = std::move(newSlot);
    }
    else
        pluginSlots.push_back(std::move(newSlot));
    attachOutputCallback();

    if (outputWasOpen && publishedSlot->runtime.hasLifecycleFault())
    {
        detachOutputCallback();
        if (slotIndex < slotCount)
            pluginSlots[static_cast<std::size_t>(slotIndex)] = std::move(replacedSlot);
        else
            pluginSlots.pop_back();
        attachOutputCallback();
        return juce::Result::fail(
            mic_daw::uiText(u8"현재 출력 장치에서 플러그인을 시작하지 못해 기존 슬롯을 유지했습니다.",
                            "The plugin could not start on the current output device, so the existing slot was kept."));
    }

    addLog((restoring ? mic_daw::uiText(u8"FX 복원: ", "FX restored: ")
                      : mic_daw::uiText(u8"FX 로드: ", "FX loaded: "))
           + juce::String(slotIndex + 1) + ". " + pluginName);
    notifyStateChanged();
    return juce::Result::ok();
}

juce::Result AudioEngine::createPluginSlot(
    const juce::File& module,
    const juce::String& preferredIdentifier,
    const juce::String& preferredFormatName,
    const juce::String& stateBase64,
    bool bypassed,
    std::unique_ptr<PluginSlot>& createdSlot)
{
    if (!module.exists())
        return juce::Result::fail(
            mic_daw::uiText(u8"선택한 플러그인 모듈을 찾을 수 없습니다.",
                            "The selected plugin module could not be found."));

    juce::OwnedArray<juce::PluginDescription> found;
    juce::AudioPluginFormat* discoveredFormat = nullptr;

    try
    {
        for (int i = 0; i < pluginFormats.getNumFormats(); ++i)
        {
            auto* format = pluginFormats.getFormat(i);
            if (format == nullptr
                || (preferredFormatName.isNotEmpty()
                    && !format->getName().equalsIgnoreCase(preferredFormatName))
                || !format->fileMightContainThisPluginType(module.getFullPathName()))
                continue;

            found.clear();
            format->findAllTypesForFile(found, module.getFullPathName());
            if (!found.isEmpty())
            {
                discoveredFormat = format;
                break;
            }
        }
    }
    catch (...)
    {
        return juce::Result::fail(
            mic_daw::uiText(u8"플러그인 모듈을 검사하는 중 예외가 발생했습니다.",
                            "An exception occurred while scanning the plugin module."));
    }

    if (discoveredFormat == nullptr || found.isEmpty())
        return juce::Result::fail(
            mic_daw::uiText(u8"선택한 경로에서 지원되는 오디오 효과를 찾지 못했습니다.",
                            "No supported audio effect was found at the selected path."));

    auto* selected = found[0];
    if (preferredIdentifier.isNotEmpty())
    {
        selected = nullptr;
        for (auto* description : found)
        {
            if (description != nullptr
                && description->createIdentifierString() == preferredIdentifier)
            {
                selected = description;
                break;
            }
        }

        if (selected == nullptr)
            return juce::Result::fail(
                mic_daw::uiText(u8"저장된 플러그인 클래스가 이 모듈에서 더 이상 발견되지 않습니다.",
                                "The saved plugin class is no longer present in this module."));
    }

    juce::String error;
    const auto outputRate = outputEndpoint.sampleRate.load();
    const auto outputBlockSize = outputEndpoint.bufferSize.load();
    const auto sampleRate = outputRate > 0.0 ? outputRate : kPreferredSampleRate;
    const auto blockSize = outputBlockSize > 0 ? outputBlockSize : kFallbackBlockSize;

    std::unique_ptr<juce::AudioPluginInstance> instance;
    try
    {
        instance = pluginFormats.createPluginInstance(*selected, sampleRate, blockSize, error);
    }
    catch (...)
    {
        return juce::Result::fail(
            mic_daw::uiText(u8"플러그인 인스턴스를 만드는 중 예외가 발생했습니다.",
                            "An exception occurred while creating the plugin instance."));
    }

    if (instance == nullptr)
        return juce::Result::fail(
            error.isNotEmpty()
                ? error
                : mic_daw::uiText(u8"플러그인 인스턴스 생성에 실패했습니다.",
                                  "Failed to create the plugin instance."));

    auto newSlot = std::make_unique<PluginSlot>(pluginChangeRevision);
    auto installResult = juce::Result::fail(
        mic_daw::uiText(u8"플러그인 초기화에 실패했습니다.", "Failed to initialize the plugin."));
    try
    {
        installResult = newSlot->runtime.install(std::move(instance));
    }
    catch (...)
    {
        return juce::Result::fail(
            mic_daw::uiText(u8"플러그인 입출력을 초기화하는 중 예외가 발생했습니다.",
                            "An exception occurred while initializing the plugin I/O."));
    }
    if (installResult.failed())
        return installResult;

    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock state;
        if (!state.fromBase64Encoding(stateBase64) || state.getSize() == 0
            || state.getSize() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return juce::Result::fail(
                mic_daw::uiText(u8"저장된 플러그인 상태 데이터가 비어 있거나 손상되었습니다.",
                                "The saved plugin state data is empty or corrupted."));

        try
        {
            const juce::ScopedLock callbackLock(newSlot->runtime.get()->getCallbackLock());
            newSlot->runtime.get()->setStateInformation(
                state.getData(), static_cast<int>(state.getSize()));
        }
        catch (...)
        {
            return juce::Result::fail(
                mic_daw::uiText(u8"저장된 플러그인 상태를 적용하는 중 예외가 발생했습니다.",
                                "An exception occurred while applying the saved plugin state."));
        }

        // Preserve the successfully applied blob even if this plugin cannot
        // immediately produce another snapshot of its restored state.
        newSlot->savedStateBase64 = stateBase64;
    }

    try
    {
        if (!newSlot->runtime.hasExpectedLayout())
            return juce::Result::fail(
                mic_daw::uiText(u8"플러그인 상태 적용 후 mono/stereo 입출력 구성이 변경되었습니다.",
                                "The mono/stereo I/O layout changed after the plugin state was applied."));

        newSlot->name = newSlot->runtime.get()->getName();
        newSlot->path = module.getFullPathName();
        newSlot->identifier = selected->createIdentifierString();
        newSlot->formatName = selected->pluginFormatName.isNotEmpty()
                                  ? selected->pluginFormatName
                                  : discoveredFormat->getName();
    }
    catch (...)
    {
        return juce::Result::fail(
            mic_daw::uiText(u8"플러그인 정보를 확인하는 중 예외가 발생했습니다.",
                            "An exception occurred while reading plugin information."));
    }

    // Capture before publishing the slot or attaching any processing callback.
    // A failed replacement must not discard the old plugin and its recovery
    // data merely because the new plugin cannot supply any saveable state.
    const auto initialState = mic_daw::capturePluginState(
        *newSlot->runtime.get(), newSlot->savedStateBase64, true);
    if (initialState.failed())
    {
        if (newSlot->savedStateBase64.isEmpty())
            return juce::Result::fail(
                mic_daw::uiText(u8"플러그인의 초기 설정을 저장할 수 없어 로드하지 않았습니다: ",
                                "The plugin was not loaded because its initial settings could not be saved: ")
                + initialState.getErrorMessage());

        addLog(mic_daw::uiText(u8"FX 설정 재확인 보류 (복원 상태 보존): ",
                               "FX settings recheck deferred (restored state preserved): ")
               + newSlot->name + " - " + initialState.getErrorMessage());
    }

    newSlot->runtime.setBypassed(bypassed);
    if (found.size() > 1 && preferredIdentifier.isEmpty())
        addLog(mic_daw::uiText(u8"여러 클래스 중 첫 번째 효과를 선택했습니다: ",
                               "Selected the first effect among multiple classes: ")
               + newSlot->name);
    createdSlot = std::move(newSlot);
    return juce::Result::ok();
}

juce::Result AudioEngine::captureChainPreset(mic_daw::ChainPreset& preset)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    mic_daw::ChainPreset candidate;
    candidate.slots.reserve(pluginSlots.size());
    const auto canBlock = !bridgeEnabled
                          && !outputEndpoint.callbackRunning.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < pluginSlots.size(); ++index)
    {
        const auto& source = *pluginSlots[index];
        const auto label = mic_daw::uiText(u8"FX 슬롯 ", "FX slot ")
                           + juce::String(static_cast<int>(index + 1)) + ": ";
        if (!source.runtime.hasPlugin())
            return juce::Result::fail(label + mic_daw::uiText(
                u8"복원되지 않은 플러그인은 새 체인으로 저장할 수 없습니다. 기존 백업을 보존하세요.",
                "An unrestored plugin cannot be saved as a new chain. Keep the existing backup."));
        mic_daw::ChainPreset::Slot slot;
        slot.path = source.path;
        slot.identifier = source.identifier;
        slot.formatName = source.formatName;
        slot.bypassed = source.runtime.isBypassed();
        const auto captured = mic_daw::capturePluginState(
            *source.runtime.get(), slot.stateBase64, canBlock);
        if (captured.failed())
            return juce::Result::fail(label + captured.getErrorMessage());
        candidate.slots.push_back(std::move(slot));
    }
    const auto checked = mic_daw::ChainPresetCodec::validate(candidate);
    if (checked.failed())
        return checked;
    preset = std::move(candidate);
    return juce::Result::ok();
}

juce::Result AudioEngine::applyChainPreset(const mic_daw::ChainPreset& preset)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto outputRate = outputEndpoint.sampleRate.load();
    const auto outputBlock = outputEndpoint.bufferSize.load();
    const auto rate = outputRate > 0.0 ? outputRate : kPreferredSampleRate;
    const auto block = outputBlock > 0 ? outputBlock : kFallbackBlockSize;
    auto result = juce::Result::ok();
    try
    {
        result = mic_daw::stageChainPreset<PluginSlot>(preset,
            [&](const mic_daw::ChainPreset::Slot& source, std::unique_ptr<PluginSlot>& slot)
            {
                const auto created = createPluginSlot(juce::File(source.path), source.identifier,
                    source.formatName, source.stateBase64, source.bypassed, slot);
                if (created.failed())
                    return created;
                // This instance is not visible to the audio callback. Even with
                // a stopped bridge, fail preparation before changing the rack.
                slot->runtime.prepare(rate, block);
                if (slot->runtime.hasLifecycleFault() || !slot->runtime.hasExpectedLayout())
                    return juce::Result::fail(mic_daw::uiText(
                        u8"플러그인을 현재 출력 형식으로 준비하지 못했습니다.",
                        "The plugin could not prepare for the current output format."));
                return juce::Result::ok();
            },
            [&](std::vector<std::unique_ptr<PluginSlot>>& staged)
            {
                bool formatChanged = false;
                {
                    // JUCE uses this lock for processing and device lifecycle
                    // callbacks. Only compare/swap here: no plugin calls, file
                    // work, allocation or retired-instance destruction.
                    const juce::ScopedLock callbackLock(outputManager.getAudioCallbackLock());
                    formatChanged = outputEndpoint.sampleRate.load() != outputRate
                                    || outputEndpoint.bufferSize.load() != outputBlock;
                    if (!formatChanged)
                        pluginSlots.swap(staged);
                }
                // Retired slots remain alive until after the callback lock was
                // drained and released; staging cleanup releases them off audio.
                return formatChanged
                    ? juce::Result::fail(mic_daw::uiText(
                          u8"체인을 준비하는 동안 출력 형식이 변경되었습니다. 다시 불러오세요.",
                          "The output format changed while preparing the chain. Load it again."))
                    : juce::Result::ok();
            });
    }
    catch (...)
    {
        return juce::Result::fail(mic_daw::uiText(
            u8"체인 준비 중 예외가 발생해 기존 체인을 유지했습니다.",
            "An exception occurred while preparing the chain; the existing chain was retained."));
    }
    if (result.failed())
        return result;
    addLog(mic_daw::uiText(u8"체인 불러오기 완료: ", "Chain loaded: ")
           + juce::String(static_cast<int>(pluginSlots.size()))
           + mic_daw::uiText(u8"개 FX", " FX"));
    notifyStateChanged();
    return juce::Result::ok();
}

juce::Result AudioEngine::captureSettingsSnapshot(
    mic_daw::SettingsSnapshot& snapshot, bool allowBlocking)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    mic_daw::SettingsSnapshot candidate = snapshot;
    auto& values = candidate.properties;
    const auto canBlock = allowBlocking && !bridgeEnabled
        && !outputEndpoint.callbackRunning.load(std::memory_order_acquire);
    try
    {
        // Remove inherited rack/legacy fields before writing the current ordered
        // rack. Search roots and other UI-owned properties are left untouched.
        const auto names = values.getAllKeys();
        for (const auto& name : names)
            if (name.startsWith("pluginSlot") || name == "pluginPath"
                || name == "pluginIdentifier" || name == "pluginState" || name == "pluginBypassed")
                values.remove(juce::StringRef(name));
        values.set("bridgeEnabled", bridgeEnabled ? "1" : "0");
        values.set("autoRecover", autoRecover ? "1" : "0");
        values.set("inputDevice", inputEndpoint.desiredDevice);
        values.set("outputDevice", outputEndpoint.desiredDevice);
        values.set("inputChannelMode", juce::String(inputChannelMode.load()));
        mic_daw::stripRetiredSettings(values);
        values.set("pluginRackVersion", "1");
        values.set("pluginRackCount", juce::String(static_cast<int>(pluginSlots.size())));
        for (std::size_t index = 0; index < pluginSlots.size(); ++index)
        {
            const auto& slot = *pluginSlots[index];
            const auto prefix = "pluginSlot" + juce::String(static_cast<int>(index));
            if (!slot.runtime.hasPlugin())
                return juce::Result::fail("FX " + juce::String(static_cast<int>(index + 1))
                    + mic_daw::uiText(" could not be restored. Keep the existing backup."));
            juce::String state;
            const auto captured = mic_daw::capturePluginState(*slot.runtime.get(), state, canBlock);
            if (captured.failed())
                return juce::Result::fail("FX " + juce::String(static_cast<int>(index + 1))
                    + ": " + captured.getErrorMessage());
            values.set(prefix + "Path", slot.path);
            values.set(prefix + "Identifier", slot.identifier);
            values.set(prefix + "Format", slot.formatName);
            values.set(prefix + "Bypassed", slot.runtime.isBypassed() ? "1" : "0");
            values.set(prefix + "State", state);
        }
        const auto checked = mic_daw::SettingsSnapshotCodec::validate(candidate);
        if (checked.failed())
            return checked;
    }
    catch (...)
    {
        return juce::Result::fail(mic_daw::uiText("The current settings could not be captured. The existing file is retained."));
    }
    snapshot = std::move(candidate);
    return juce::Result::ok();
}

juce::Result AudioEngine::applySettingsSnapshot(
    const mic_daw::SettingsSnapshot& snapshot,
    const std::function<juce::Result()>& beforeCommit)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto outputRate = outputEndpoint.sampleRate.load();
    const auto outputBlock = outputEndpoint.bufferSize.load();
    const auto rate = outputRate > 0.0 ? outputRate : kPreferredSampleRate;
    const auto block = outputBlock > 0 ? outputBlock : kFallbackBlockSize;
    const auto result = mic_daw::stageSettingsSnapshot<PluginSlot>(
        snapshot,
        [&](const mic_daw::ChainPreset::Slot& source, std::unique_ptr<PluginSlot>& slot)
        {
            const auto created = createPluginSlot(juce::File(source.path), source.identifier,
                source.formatName, source.stateBase64, source.bypassed, slot);
            if (created.failed())
                return created;
            slot->runtime.prepare(rate, block);
            if (slot->runtime.hasLifecycleFault() || !slot->runtime.hasExpectedLayout())
                return juce::Result::fail(mic_daw::uiText("The plugin could not prepare for the output format."));
            return juce::Result::ok();
        },
        [&] { return beforeCommit ? beforeCommit() : juce::Result::ok(); },
        [&](mic_daw::DecodedSettingsSnapshot& decoded,
            std::vector<std::unique_ptr<PluginSlot>>& slots)
        {
            // All fallible data/plugin preparation and disk work completed.
            // Stop/drain both callbacks before replacing any shared lifetime.
            setBridgeEnabled(false);
            detachInputCallback();
            detachOutputCallback();
            inputEndpoint.disarmCallback();
            outputEndpoint.disarmCallback();
            inputManager.closeAudioDevice();
            outputManager.closeAudioDevice();
            inputEndpoint.recovery.stop();
            outputEndpoint.recovery.stop();
            pluginSlots.swap(slots);
            inputEndpoint.desiredDevice = std::move(decoded.inputDevice);
            outputEndpoint.desiredDevice = std::move(decoded.outputDevice);
            inputEndpoint.lastError.clear();
            outputEndpoint.lastError.clear();
            inputEndpoint.retryCount = 0;
            outputEndpoint.retryCount = 0;
            inputChannelMode.store(decoded.inputChannelMode, std::memory_order_release);
            autoRecover = decoded.autoRecover;
            inputEndpoint.recovery.setAutoRecoveryEnabled(autoRecover);
            outputEndpoint.recovery.setAutoRecoveryEnabled(autoRecover);

            ring.requestClear();
            attachInputCallback();
            attachOutputCallback();
            // Real devices may be absent or have a different format. Retain the
            // imported desired configuration and let normal recovery/bypass run.
            setBridgeEnabled(decoded.bridgeEnabled);
        });
    if (result.wasOk())
    {
        addLog(mic_daw::uiText("Full settings loaded."));
        notifyStateChanged();
    }
    return result;
}

void AudioEngine::clearPlugin(int slotIndex)
{
    if (!juce::isPositiveAndBelow(slotIndex, static_cast<int>(pluginSlots.size())))
        return;

    const auto name = pluginSlots[static_cast<std::size_t>(slotIndex)]->name;
    detachOutputCallback();
    pluginSlots.erase(pluginSlots.begin() + slotIndex);
    attachOutputCallback();
    addLog(mic_daw::uiText(u8"FX 제거: ", "FX removed: ") + name);
    notifyStateChanged();
}

void AudioEngine::clearAllPlugins()
{
    if (pluginSlots.empty())
        return;

    detachOutputCallback();
    pluginSlots.clear();
    attachOutputCallback();
    addLog(mic_daw::uiText(u8"모든 FX를 제거했습니다.", "All FX removed."));
    notifyStateChanged();
}

bool AudioEngine::movePlugin(int slotIndex, int destinationIndex)
{
    const auto count = static_cast<int>(pluginSlots.size());
    if (!juce::isPositiveAndBelow(slotIndex, count)
        || !juce::isPositiveAndBelow(destinationIndex, count)
        || slotIndex == destinationIndex)
        return false;

    detachOutputCallback();
    std::swap(pluginSlots[static_cast<std::size_t>(slotIndex)],
              pluginSlots[static_cast<std::size_t>(destinationIndex)]);
    attachOutputCallback();
    addLog(mic_daw::uiText(u8"FX 순서를 변경했습니다: ", "FX order changed: ")
           + juce::String(slotIndex + 1)
           + mic_daw::uiText(u8" ↔ ", " <-> ") + juce::String(destinationIndex + 1));
    notifyStateChanged();
    return true;
}

void AudioEngine::setPluginBypassed(int slotIndex, bool bypassed)
{
    if (!juce::isPositiveAndBelow(slotIndex, static_cast<int>(pluginSlots.size())))
        return;

    pluginSlots[static_cast<std::size_t>(slotIndex)]->runtime.setBypassed(bypassed);
    notifyStateChanged();
}

std::vector<AudioEngine::PluginSlotSnapshot> AudioEngine::getPluginSlots() const
{
    std::vector<PluginSlotSnapshot> snapshots;
    snapshots.reserve(pluginSlots.size());

    for (const auto& slot : pluginSlots)
    {
        PluginSlotSnapshot snapshot;
        snapshot.name = slot->name;
        if (snapshot.name.isEmpty())
            snapshot.name = juce::File(slot->path).getFileNameWithoutExtension();
        if (snapshot.name.isEmpty())
            snapshot.name = mic_daw::uiText(u8"복원되지 않은 FX", "Unrestored FX");
        snapshot.path = slot->path;
        snapshot.formatName = slot->formatName;
        snapshot.restoreError = slot->restoreError;
        snapshot.available = slot->runtime.hasPlugin();
        snapshot.bypassed = slot->runtime.isBypassed();
        snapshot.reportedLatencySamples = slot->runtime.getReportedLatencySamples();
        snapshots.push_back(std::move(snapshot));
    }

    return snapshots;
}

juce::AudioPluginInstance* AudioEngine::getPlugin(int slotIndex) const noexcept
{
    if (!juce::isPositiveAndBelow(slotIndex, static_cast<int>(pluginSlots.size())))
        return nullptr;

    return pluginSlots[static_cast<std::size_t>(slotIndex)]->runtime.get();
}

void AudioEngine::detachInputCallback()
{
    if (!inputCallbackRegistered)
        return;

    inputManager.removeAudioCallback(&inputCallback);
    inputCallbackRegistered = false;
}

void AudioEngine::attachInputCallback()
{
    if (inputCallbackRegistered)
        return;

    inputManager.addAudioCallback(&inputCallback);
    inputCallbackRegistered = true;
}

void AudioEngine::detachOutputCallback()
{
    if (!outputCallbackRegistered)
        return;

    outputManager.removeAudioCallback(&outputCallback);
    outputCallbackRegistered = false;
}

void AudioEngine::attachOutputCallback()
{
    if (outputCallbackRegistered)
        return;

    outputManager.addAudioCallback(&outputCallback);
    outputCallbackRegistered = true;
}

void AudioEngine::preparePluginChain(double sampleRate, int maximumBlockSize)
{
    for (auto& slot : pluginSlots)
        slot->runtime.prepare(sampleRate, maximumBlockSize);
}

void AudioEngine::releasePluginChain()
{
    for (auto iterator = pluginSlots.rbegin(); iterator != pluginSlots.rend(); ++iterator)
        (*iterator)->runtime.release();
}

AudioEngine::EndpointSnapshot AudioEngine::getInputSnapshot() const
{
    EndpointSnapshot snapshot;
    snapshot.state = toPublicState(inputEndpoint, bridgeMayUseDevices());
    snapshot.desiredDevice = inputEndpoint.desiredDevice;
    snapshot.detail = inputEndpoint.lastError.isNotEmpty()
                        ? inputEndpoint.lastError
                        : endpointStateName(snapshot.state);
    snapshot.sampleRate = inputEndpoint.sampleRate.load();
    snapshot.bufferSize = inputEndpoint.bufferSize.load();
    snapshot.retryCount = inputEndpoint.retryCount;

    if (auto* device = inputManager.getCurrentAudioDevice())
        snapshot.activeDevice = inputManager.getAudioDeviceSetup().inputDeviceName;

    return snapshot;
}

AudioEngine::EndpointSnapshot AudioEngine::getOutputSnapshot() const
{
    EndpointSnapshot snapshot;
    snapshot.state = toPublicState(outputEndpoint, bridgeMayUseDevices());
    snapshot.desiredDevice = outputEndpoint.desiredDevice;
    snapshot.detail = outputEndpoint.lastError.isNotEmpty()
                        ? outputEndpoint.lastError
                        : endpointStateName(snapshot.state);
    snapshot.sampleRate = outputEndpoint.sampleRate.load();
    snapshot.bufferSize = outputEndpoint.bufferSize.load();
    snapshot.retryCount = outputEndpoint.retryCount;

    if (auto* device = outputManager.getCurrentAudioDevice())
        snapshot.activeDevice = outputManager.getAudioDeviceSetup().outputDeviceName;

    return snapshot;
}

AudioEngine::MeterSnapshot AudioEngine::getMeterSnapshot() const noexcept
{
    MeterSnapshot snapshot;
    // A callback can race a device-error notification while publishing its
    // final block. Gate the read as well as clearing the atomics on stop/error
    // so disconnected or unselected endpoints never display stale levels.
    const auto inputActive = inputEndpoint.callbackRunning.load(std::memory_order_acquire)
                             && !inputEndpoint.callbackError.load(std::memory_order_acquire)
                             && inputEndpoint.callbackIsAuthorized();
    const auto outputActive = outputEndpoint.callbackRunning.load(std::memory_order_acquire)
                              && !outputEndpoint.callbackError.load(std::memory_order_acquire)
                              && outputEndpoint.callbackIsAuthorized();
    const auto pendingInputPeak = inputPeak.take();
    const auto pendingOutputPeak = outputPeak.take();
    snapshot.inputPeak = inputActive ? pendingInputPeak : 0.0f;
    snapshot.outputPeak = outputActive ? pendingOutputPeak : 0.0f;
    snapshot.inputRms = inputActive ? inputRms.load(std::memory_order_relaxed) : 0.0f;
    snapshot.outputRms = outputActive ? outputRms.load(std::memory_order_relaxed) : 0.0f;
    snapshot.ringFrames = ringOccupancy.load();
    snapshot.ringTargetFrames = ringTarget.load();
    snapshot.driftCorrectionPpm = correctionPpm.load();
    snapshot.underruns = underruns.load();
    snapshot.overruns = ring.getOverrunCount();
    snapshot.hardResyncs = hardResyncs.load();
    for (const auto& slot : pluginSlots)
        snapshot.pluginLatencySamples += slot->runtime.getReportedLatencySamples();
    return snapshot;
}

juce::StringArray AudioEngine::getRecentLog() const
{
    const juce::ScopedLock lock(logLock);
    return recentLog;
}

void AudioEngine::restoreSettings(juce::PropertiesFile& settings)
{
    jassert(pluginSlots.empty());
    if (settings.containsKey("correctionProfileJson") || settings.containsKey("correctionEnabled"))
        addLog(mic_daw::uiText("Built-in mic correction was removed in 0.4.4. The old profile is ignored; original settings are preserved in the startup backup."));
    autoRecover = settings.getBoolValue("autoRecover", true);
    inputEndpoint.recovery.setAutoRecoveryEnabled(autoRecover);
    outputEndpoint.recovery.setAutoRecoveryEnabled(autoRecover);
    inputEndpoint.desiredDevice = settings.getValue("inputDevice");
    outputEndpoint.desiredDevice = settings.getValue("outputDevice");
    inputChannelMode.store(juce::jlimit(0, 2, settings.getIntValue("inputChannelMode", 0)));

    refreshDeviceLists();

    if (inputEndpoint.desiredDevice.isEmpty())
        inputEndpoint.desiredDevice = getDefaultInputDevice();
    if (outputEndpoint.desiredDevice.isEmpty())
        outputEndpoint.desiredDevice = findPreferredCableOutput();


    if (settings.containsKey("pluginRackCount"))
    {
        const auto savedCount = juce::jlimit(
            0, kMaxPluginSlots, settings.getIntValue("pluginRackCount", 0));
        for (int savedIndex = 0; savedIndex < savedCount; ++savedIndex)
        {
            const auto prefix = "pluginSlot" + juce::String(savedIndex);
            const auto path = settings.getValue(prefix + "Path");
            const auto identifier = settings.getValue(prefix + "Identifier");
            const auto formatName = settings.getValue(prefix + "Format");
            const auto stateBase64 = settings.getValue(prefix + "State");
            const auto bypassed = settings.getBoolValue(prefix + "Bypassed", false);
            const auto result = path.isNotEmpty()
                                    ? loadPluginInternal(
                                          static_cast<int>(pluginSlots.size()),
                                          juce::File(path),
                                          identifier,
                                          formatName,
                                          stateBase64,
                                          bypassed,
                                          true)
                                    : juce::Result::fail(mic_daw::uiText(
                                          u8"저장된 플러그인 경로가 비어 있습니다.",
                                          "The saved plugin path is empty."));
            if (result.failed())
            {
                auto unresolved = std::make_unique<PluginSlot>(pluginChangeRevision);
                unresolved->path = path;
                unresolved->identifier = identifier;
                unresolved->formatName = formatName;
                unresolved->savedStateBase64 = stateBase64;
                unresolved->restoreError = result.getErrorMessage();
                unresolved->runtime.setBypassed(bypassed);
                pluginSlots.push_back(std::move(unresolved));
                addLog(mic_daw::uiText(u8"저장된 FX ", "Saved FX ")
                       + juce::String(savedIndex + 1)
                       + mic_daw::uiText(u8" 복원 실패: ", " restore failed: ")
                       + result.getErrorMessage());
            }
        }
    }
    else
    {
        const auto legacyPath = settings.getValue("pluginPath");
        if (legacyPath.isNotEmpty())
        {
            const auto result = loadPluginInternal(
                0,
                juce::File(legacyPath),
                settings.getValue("pluginIdentifier"),
                {},
                settings.getValue("pluginState"),
                settings.getBoolValue("pluginBypassed", false),
                true);
            if (result.failed())
            {
                auto unresolved = std::make_unique<PluginSlot>(pluginChangeRevision);
                unresolved->path = legacyPath;
                unresolved->identifier = settings.getValue("pluginIdentifier");
                unresolved->formatName = "VST3";
                unresolved->savedStateBase64 = settings.getValue("pluginState");
                unresolved->restoreError = result.getErrorMessage();
                unresolved->runtime.setBypassed(
                    settings.getBoolValue("pluginBypassed", false));
                pluginSlots.push_back(std::move(unresolved));
                addLog(mic_daw::uiText(u8"기존 VST3 설정 복원 실패: ",
                                       "Legacy VST3 settings restore failed: ")
                       + result.getErrorMessage());
            }
        }
    }

    setBridgeEnabled(settings.getBoolValue("bridgeEnabled", true));
}

juce::Result AudioEngine::saveSettings(juce::PropertiesFile& settings, bool allowBlocking)
{
    juce::StringArray snapshotErrors;

    // Even an accidental allowBlocking=true from a live caller must not wait
    // for a running audio callback. A final save can wait after close has
    // synchronously stopped the output device and its processing callback.
    const auto canBlock = allowBlocking && !bridgeEnabled
                          && !outputEndpoint.callbackRunning.load(std::memory_order_acquire);

    for (std::size_t index = 0; index < pluginSlots.size(); ++index)
    {
        auto& slot = *pluginSlots[index];
        if (!slot.runtime.hasPlugin())
            continue;

        const auto result = mic_daw::capturePluginState(
            *slot.runtime.get(), slot.savedStateBase64, canBlock);
        if (result.failed())
            snapshotErrors.add("FX " + juce::String(static_cast<int>(index) + 1)
                               + " (" + slot.name + "): " + result.getErrorMessage());
    }

    // All rack mutations and saves run on the message thread. Keep the whole
    // metadata/state update locked so another reader or PropertiesFile save
    // cannot observe new ordering paired with stale index-based state.
    const juce::ScopedLock settingsLock(settings.getLock());
    settings.setValue("bridgeEnabled", bridgeEnabled);
    settings.setValue("autoRecover", autoRecover);
    settings.setValue("inputDevice", inputEndpoint.desiredDevice);
    settings.setValue("outputDevice", outputEndpoint.desiredDevice);
    settings.setValue("inputChannelMode", inputChannelMode.load());
    mic_daw::stripRetiredSettings(settings);
    settings.setValue("pluginRackVersion", 1);
    settings.setValue("pluginRackCount", static_cast<int>(pluginSlots.size()));

    for (int index = 0; index < kMaxPluginSlots; ++index)
    {
        const auto prefix = "pluginSlot" + juce::String(index);
        if (!juce::isPositiveAndBelow(index, static_cast<int>(pluginSlots.size())))
        {
            settings.removeValue(prefix + "Path");
            settings.removeValue(prefix + "Identifier");
            settings.removeValue(prefix + "Format");
            settings.removeValue(prefix + "Bypassed");
            settings.removeValue(prefix + "State");
            continue;
        }

        const auto& slot = *pluginSlots[static_cast<std::size_t>(index)];
        settings.setValue(prefix + "Path", slot.path);
        settings.setValue(prefix + "Identifier", slot.identifier);
        settings.setValue(prefix + "Format", slot.formatName);
        settings.setValue(prefix + "Bypassed", slot.runtime.isBypassed());

        settings.setValue(prefix + "State", slot.savedStateBase64);
    }

    settings.removeValue("pluginPath");
    settings.removeValue("pluginIdentifier");
    settings.removeValue("pluginBypassed");
    settings.removeValue("pluginState");

    return snapshotErrors.isEmpty()
               ? juce::Result::ok()
               : juce::Result::fail(snapshotErrors.joinIntoString("\n"));
}

//==============================================================================
void AudioEngine::timerCallback()
{
    const auto currentMs = nowMs();
    const auto steadyNow = mic_daw::RecoveryState::Clock::now();

    if (deviceListDirty || currentMs >= nextDeviceScanMs)
    {
        const auto inputWasPresent = inputDevices.contains(inputEndpoint.desiredDevice);
        const auto outputWasPresent = outputDevices.contains(outputEndpoint.desiredDevice);
        refreshDeviceLists();

        if (bridgeMayUseDevices())
        {
            if (inputEndpoint.recovery.status() == mic_daw::RecoveryState::Status::waiting
                && !inputWasPresent
                && inputDevices.contains(inputEndpoint.desiredDevice))
                requestImmediateRetry(inputEndpoint);

            if (outputEndpoint.recovery.status() == mic_daw::RecoveryState::Status::waiting
                && !outputWasPresent
                && outputDevices.contains(outputEndpoint.desiredDevice))
                requestImmediateRetry(outputEndpoint);
        }
    }

    checkEndpointHealth(inputEndpoint, true, currentMs);
    checkEndpointHealth(outputEndpoint, false, currentMs);

    if (bridgeMayUseDevices() && inputEndpoint.recovery.poll(steadyNow))
        attemptInputOpen();
    if (bridgeMayUseDevices() && outputEndpoint.recovery.poll(steadyNow))
        attemptOutputOpen();

    bool pluginFaulted = false;
    for (std::size_t index = 0; index < pluginSlots.size(); ++index)
    {
        auto& slot = *pluginSlots[index];
        if (!slot.runtime.consumeFault())
            continue;

        const auto name = slot.name.isNotEmpty()
                              ? slot.name
                              : mic_daw::uiText(u8"알 수 없는 FX", "Unknown FX");
        addLog("FX " + juce::String(index + 1) + " (" + name
               + mic_daw::uiText(
                   u8") 처리 예외를 감지해 dry bypass로 격리했습니다.",
                   ") threw while processing and was isolated with dry bypass."));
        pluginFaulted = true;
    }

    if (pluginFaulted)
        notifyStateChanged();



    sendChangeMessage();
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster*)
{
    deviceListDirty = true;
}

void AudioEngine::attemptInputOpen()
{
    if (!bridgeMayUseDevices() || inputEndpoint.desiredDevice.isEmpty())
    {
        inputEndpoint.lastError = inputEndpoint.desiredDevice.isEmpty()
                                    ? mic_daw::uiText(u8"입력 장치를 선택하세요.",
                                                      "Select an input device.")
                                    : juce::String{};
        inputEndpoint.recovery.stop();
        inputManager.closeAudioDevice();
        notifyStateChanged();
        return;
    }

    inputEndpoint.disarmCallback();
    inputManager.closeAudioDevice();
    ring.requestClear();
    refreshDeviceLists();
    if (!inputDevices.contains(inputEndpoint.desiredDevice))
    {
        scheduleFailure(inputEndpoint,
                        mic_daw::uiText(u8"선택한 마이크가 연결되기를 기다리는 중입니다.",
                                        "Waiting for the selected microphone to connect."),
                        false);
        return;
    }

    const auto setup = mic_daw::makeInputDeviceSetup(inputEndpoint.desiredDevice,
                                                    kPreferredSampleRate);

    inputEndpoint.armCallbackFor(inputEndpoint.desiredDevice);
    inputEndpoint.callbackError.store(false);
    const auto error = inputManager.setAudioDeviceSetup(setup, true);

    if (error.isNotEmpty())
    {
        scheduleFailure(inputEndpoint,
                        mic_daw::uiText(u8"마이크 열기 실패: ",
                                        "Failed to open microphone: ")
                            + error,
                        true);
        return;
    }

    inputEndpoint.lastError.clear();
    inputEndpoint.lastObservedCallbackMs = inputEndpoint.lastCallbackMs.load();
    static_cast<void>(inputEndpoint.recovery.recoverySucceeded(
        mic_daw::RecoveryState::Clock::now()));
    addLog(mic_daw::uiText(u8"마이크 연결됨: ", "Microphone connected: ")
           + inputEndpoint.desiredDevice);
    notifyStateChanged();
}

void AudioEngine::attemptOutputOpen()
{
    if (!bridgeMayUseDevices() || outputEndpoint.desiredDevice.isEmpty())
    {
        outputEndpoint.lastError = outputEndpoint.desiredDevice.isEmpty()
                                     ? mic_daw::uiText(
                                           u8"출력 장치로 CABLE Input을 선택하세요.",
                                           "Select CABLE Input as the output device.")
                                     : juce::String{};
        outputEndpoint.recovery.stop();
        outputManager.closeAudioDevice();
        notifyStateChanged();
        return;
    }

    outputEndpoint.disarmCallback();
    outputManager.closeAudioDevice();
    ring.requestClear();
    refreshDeviceLists();
    if (!outputDevices.contains(outputEndpoint.desiredDevice))
    {
        scheduleFailure(outputEndpoint,
                        mic_daw::uiText(u8"선택한 출력 장치가 연결되기를 기다리는 중입니다.",
                                        "Waiting for the selected output device to connect."),
                        false);
        return;
    }

    const auto setup = mic_daw::makeOutputDeviceSetup(outputEndpoint.desiredDevice,
                                                     kPreferredSampleRate);

    outputEndpoint.armCallbackFor(outputEndpoint.desiredDevice);
    outputEndpoint.callbackError.store(false);
    const auto error = outputManager.setAudioDeviceSetup(setup, true);

    if (error.isNotEmpty())
    {
        scheduleFailure(outputEndpoint,
                        mic_daw::uiText(u8"출력 열기 실패: ",
                                        "Failed to open output: ")
                            + error,
                        true);
        return;
    }

    outputEndpoint.lastError.clear();
    outputEndpoint.lastObservedCallbackMs = outputEndpoint.lastCallbackMs.load();
    static_cast<void>(outputEndpoint.recovery.recoverySucceeded(
        mic_daw::RecoveryState::Clock::now()));
    addLog(mic_daw::uiText(u8"출력 연결됨: ", "Output connected: ")
           + outputEndpoint.desiredDevice);
    notifyStateChanged();
}

void AudioEngine::scheduleFailure(EndpointControl& endpoint,
                                  const juce::String& error,
                                  bool closeDevice)
{
    if (closeDevice)
    {
        if (&endpoint == &inputEndpoint)
        {
            inputEndpoint.disarmCallback();
            inputManager.closeAudioDevice();
        }
        else
        {
            outputEndpoint.disarmCallback();
            outputManager.closeAudioDevice();
        }
    }

    endpoint.lastError = error;
    ++endpoint.retryCount;
    static_cast<void>(endpoint.recovery.recoveryFailed(
        mic_daw::RecoveryState::Clock::now()));
    addLog(error);
    notifyStateChanged();
}

void AudioEngine::requestImmediateRetry(EndpointControl& endpoint)
{
    if (!bridgeMayUseDevices() || !endpoint.recovery.requestImmediateRetry())
        return;

    if (&endpoint == &inputEndpoint)
        attemptInputOpen();
    else
        attemptOutputOpen();
}

void AudioEngine::checkEndpointHealth(EndpointControl& endpoint,
                                      bool isInput,
                                      std::int64_t currentMs)
{
    if (!bridgeMayUseDevices())
        return;

    const auto steadyNow = mic_daw::RecoveryState::Clock::now();
    const auto callbackMs = endpoint.lastCallbackMs.load(std::memory_order_relaxed);
    if (callbackMs > endpoint.lastObservedCallbackMs)
    {
        endpoint.lastObservedCallbackMs = callbackMs;
        const auto callbackAgeMs = std::max<std::int64_t>(0, currentMs - callbackMs);
        endpoint.recovery.recordHeartbeat(
            steadyNow - std::chrono::milliseconds(callbackAgeMs));
    }

    const auto wasRunning = endpoint.recovery.status()
                            == mic_daw::RecoveryState::Status::running;
    const auto activeDevice = isInput
                                  ? inputManager.getAudioDeviceSetup().inputDeviceName
                                  : outputManager.getAudioDeviceSetup().outputDeviceName;
    const auto desiredPresent = isInput
                                    ? inputDevices.contains(endpoint.desiredDevice)
                                    : outputDevices.contains(endpoint.desiredDevice);
    const auto wrongDevice = wasRunning && activeDevice != endpoint.desiredDevice;
    const auto desiredMissing = wasRunning && !desiredPresent;
    const auto unauthorizedCallback = wasRunning && !endpoint.callbackIsAuthorized();
    const auto timedOut = wasRunning && endpoint.recovery.heartbeatTimedOut(steadyNow);
    if (wasRunning && !wrongDevice && !desiredMissing && !unauthorizedCallback && !timedOut
        && endpoint.callbackRunning.load()
        && endpoint.recovery.confirmStable(steadyNow))
        endpoint.retryCount = 0;

    const auto callbackSignalled = endpoint.callbackError.exchange(false);
    const auto signalledError = wasRunning && callbackSignalled;
    const auto unexpectedlyStopped = wasRunning && !endpoint.callbackRunning.load();

    if (!wrongDevice && !desiredMissing && !unauthorizedCallback && !timedOut
        && !signalledError && !unexpectedlyStopped)
        return;

    if (wrongDevice || unauthorizedCallback)
        endpoint.lastError = isInput
                                 ? mic_daw::uiText(
                                       u8"선택하지 않은 마이크로 대체 연결되어 복구합니다.",
                                       "A different microphone was connected as a fallback; recovering.")
                                 : mic_daw::uiText(
                                       u8"선택하지 않은 출력 장치로 대체 연결되어 복구합니다.",
                                       "A different output device was connected as a fallback; recovering.");
    else if (desiredMissing)
        endpoint.lastError = isInput
                                 ? mic_daw::uiText(u8"선택한 마이크가 사라져 복구합니다.",
                                                   "The selected microphone disappeared; recovering.")
                                 : mic_daw::uiText(u8"선택한 출력 장치가 사라져 복구합니다.",
                                                   "The selected output device disappeared; recovering.");
    else
        endpoint.lastError = isInput
                                 ? (timedOut
                                        ? mic_daw::uiText(u8"마이크 응답이 없어 복구합니다.",
                                                          "The microphone stopped responding; recovering.")
                                        : mic_daw::uiText(u8"마이크 스트림이 중단되어 복구합니다.",
                                                          "The microphone stream stopped; recovering."))
                                 : (timedOut
                                        ? mic_daw::uiText(u8"출력 응답이 없어 복구합니다.",
                                                          "The output stopped responding; recovering.")
                                        : mic_daw::uiText(u8"출력 스트림이 중단되어 복구합니다.",
                                                          "The output stream stopped; recovering."));

    if (isInput)
    {
        inputEndpoint.disarmCallback();
        inputManager.closeAudioDevice();
        ring.requestClear();
    }
    else
    {
        outputEndpoint.disarmCallback();
        outputManager.closeAudioDevice();
    }

    if (!autoRecover)
    {
        endpoint.recovery.stop();
        addLog(endpoint.lastError);
        notifyStateChanged();
        return;
    }

    ++endpoint.retryCount;
    static_cast<void>(endpoint.recovery.deviceLost(
        mic_daw::RecoveryState::Clock::now()));
    addLog(endpoint.lastError);
    notifyStateChanged();
}

AudioEngine::EndpointState AudioEngine::toPublicState(const EndpointControl& endpoint,
                                                       bool enabled) noexcept
{
    if (!enabled)
        return EndpointState::stopped;

    switch (endpoint.recovery.status())
    {
        case mic_daw::RecoveryState::Status::stopped:
            return endpoint.lastError.isNotEmpty() ? EndpointState::error : EndpointState::stopped;
        case mic_daw::RecoveryState::Status::waiting:
            return EndpointState::waiting;
        case mic_daw::RecoveryState::Status::recovering:
            return EndpointState::recovering;
        case mic_daw::RecoveryState::Status::running:
            return EndpointState::running;
    }

    return EndpointState::error;
}

void AudioEngine::renderInput(juce::AudioBuffer<float>& destination,
                              int numSamples) noexcept
{
    destination.clear(0, numSamples);

    if (ring.applyPendingClear())
    {
        resamplePosition = 0.0;
        inputPrimed = false;
        inputFadeGain = 0.0f;
    }

    const auto inputRate = inputEndpoint.sampleRate.load(std::memory_order_relaxed);
    const auto outputRate = outputEndpoint.sampleRate.load(std::memory_order_relaxed);
    auto available = ring.available();
    ringOccupancy.store(static_cast<int>(available), std::memory_order_relaxed);

    if (inputRate <= 0.0 || outputRate <= 0.0)
    {
        correctionPpm.store(0.0, std::memory_order_relaxed);
        return;
    }

    if (available < 2)
    {
        correctionPpm.store(0.0, std::memory_order_relaxed);
        if (mic_daw::shouldCountInputUnderrun(inputRate, outputRate, available, inputPrimed))
        {
            inputPrimed = false;
            inputFadeGain = 0.0f;
            underruns.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    const auto target = static_cast<std::size_t>(std::clamp(
        inputRate * 0.06,
        256.0,
        static_cast<double>(ring.capacity() / 4)));
    ringTarget.store(static_cast<int>(target), std::memory_order_relaxed);

    const auto highWatermark = std::min(ring.capacity() - 1, target * 2);
    if (available > highWatermark)
    {
        ring.consume(available - target);
        available = ring.available();
        ringOccupancy.store(static_cast<int>(available), std::memory_order_relaxed);
        resamplePosition = 0.0;
        inputPrimed = false;
        inputFadeGain = 0.0f;
        hardResyncs.fetch_add(1, std::memory_order_relaxed);
    }

    if (!inputPrimed)
    {
        if (available < target)
            return;

        inputPrimed = true;
        resamplePosition = 0.0;
        inputFadeGain = 0.0f;
    }

    const auto normalizedError = (static_cast<double>(available) - static_cast<double>(target))
                                 / std::max(1.0, static_cast<double>(target));
    const auto ppm = std::clamp(normalizedError * 1000.0, -1000.0, 1000.0);
    const auto ratio = (inputRate / outputRate) * (1.0 + ppm * 0.000001);
    correctionPpm.store(ppm, std::memory_order_relaxed);

    auto* left = destination.getWritePointer(0);
    auto* right = destination.getWritePointer(std::min(1, destination.getNumChannels() - 1));
    const auto fadeStep = static_cast<float>(1.0 / std::max(1.0, outputRate * 0.02));
    int produced = 0;

    for (; produced < numSamples; ++produced)
    {
        const auto index = static_cast<std::size_t>(resamplePosition);
        if (index + 1 >= available)
            break;

        const auto fraction = static_cast<float>(resamplePosition - static_cast<double>(index));
        const auto left0 = ring.peek(0, index);
        const auto left1 = ring.peek(0, index + 1);
        const auto right0 = ring.peek(1, index);
        const auto right1 = ring.peek(1, index + 1);

        inputFadeGain = std::min(1.0f, inputFadeGain + fadeStep);
        left[produced] = (left0 + fraction * (left1 - left0)) * inputFadeGain;
        right[produced] = (right0 + fraction * (right1 - right0)) * inputFadeGain;
        resamplePosition += ratio;
    }

    const auto consumed = static_cast<std::size_t>(resamplePosition);
    ring.consume(consumed);
    resamplePosition -= static_cast<double>(consumed);

    if (produced < numSamples)
    {
        inputPrimed = false;
        inputFadeGain = 0.0f;
        underruns.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioEngine::processOutput(juce::AudioBuffer<float>& buffer,
                                int numSamples) noexcept
{
    for (auto& slot : pluginSlots)
        slot->runtime.process(buffer, numSamples);
}

void AudioEngine::addLog(const juce::String& line)
{
    const juce::ScopedLock lock(logLock);
    recentLog.add(juce::Time::getCurrentTime().formatted("%H:%M:%S") + "  " + line);
    while (recentLog.size() > 100)
        recentLog.remove(0);
}

void AudioEngine::notifyStateChanged()
{
    sendChangeMessage();
}

juce::AudioIODeviceType* AudioEngine::getWasapiType(
    SharedWasapiDeviceManager& manager) const
{
    const auto& types = manager.getAvailableDeviceTypes();
    return types.isEmpty() ? nullptr : types[0];
}

std::int64_t AudioEngine::nowMs() noexcept
{
    return static_cast<std::int64_t>(juce::Time::getMillisecondCounterHiRes());
}
