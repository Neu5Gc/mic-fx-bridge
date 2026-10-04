#pragma once

#include <JuceHeader.h>

#include "RecoveryState.h"
#include "RmsMeter.h"
#include "PeakAccumulator.h"
#include "ChainPreset.h"
#include "SettingsSnapshot.h"
#include "LegacySettings.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <vector>

class AudioEngine final : public juce::ChangeBroadcaster,
                          private juce::Timer,
                          private juce::ChangeListener
{
public:
    static constexpr int kMaxPluginSlots = 8;

    enum class EndpointState
    {
        stopped,
        waiting,
        recovering,
        running,
        error
    };

    struct EndpointSnapshot
    {
        EndpointState state = EndpointState::stopped;
        juce::String desiredDevice;
        juce::String activeDevice;
        juce::String detail;
        double sampleRate = 0.0;
        int bufferSize = 0;
        int retryCount = 0;
    };

    struct MeterSnapshot
    {
        float inputPeak = 0.0f;
        float outputPeak = 0.0f;
        // Linear, uncalibrated RMS: full-scale DC = 1, full-scale sine =
        // sqrt(0.5). Convert with 20*log10(value) for mathematical dBFS RMS.
        float inputRms = 0.0f;
        float outputRms = 0.0f;
        int ringFrames = 0;
        int ringTargetFrames = 0;
        double driftCorrectionPpm = 0.0;
        std::uint64_t underruns = 0;
        std::uint64_t overruns = 0;
        std::uint64_t hardResyncs = 0;
        int pluginLatencySamples = 0;
    };

    struct PluginSlotSnapshot
    {
        juce::String name;
        juce::String path;
        juce::String formatName;
        juce::String restoreError;
        bool available = false;
        bool bypassed = false;
        int reportedLatencySamples = 0;
    };

    AudioEngine();
    ~AudioEngine() override;

    void setBridgeEnabled(bool shouldRun);
    [[nodiscard]] bool isBridgeEnabled() const noexcept { return bridgeEnabled; }

    void setAutoRecover(bool shouldRecover);
    [[nodiscard]] bool isAutoRecoverEnabled() const noexcept { return autoRecover; }

    void setInputDevice(const juce::String& name);
    void setOutputDevice(const juce::String& name);
    [[nodiscard]] const juce::String& getDesiredInputDevice() const noexcept;
    [[nodiscard]] const juce::String& getDesiredOutputDevice() const noexcept;

    // 0: input 1 as mono, 1: input 2 as mono, 2: input 1/2 stereo.
    void setInputChannelMode(int mode);
    [[nodiscard]] int getInputChannelMode() const noexcept;

    void refreshDeviceLists();
    [[nodiscard]] juce::StringArray getInputDevices() const;
    [[nodiscard]] juce::StringArray getOutputDevices() const;
    [[nodiscard]] juce::String getDefaultInputDevice() const;
    [[nodiscard]] juce::String findPreferredCableOutput() const;

    void retryNow();
    [[nodiscard]] juce::Result loadPlugin(int slotIndex, const juce::File& module);
    void clearPlugin(int slotIndex);
    void clearAllPlugins();
    [[nodiscard]] bool movePlugin(int slotIndex, int destinationIndex);
    void setPluginBypassed(int slotIndex, bool bypassed);
    [[nodiscard]] std::vector<PluginSlotSnapshot> getPluginSlots() const;
    [[nodiscard]] juce::AudioPluginInstance* getPlugin(int slotIndex) const noexcept;
    // Message thread only. Failed captures leave the destination unchanged;
    // failed imports leave the complete live rack unchanged.
    [[nodiscard]] juce::Result captureChainPreset(mic_daw::ChainPreset& preset);
    [[nodiscard]] juce::Result applyChainPreset(const mic_daw::ChainPreset& preset);
    // Seed properties with UI preferences first. Fresh capture failure leaves
    // the snapshot unchanged; no stale plugin blob is exported as a new save.
    [[nodiscard]] juce::Result captureSettingsSnapshot(
        mic_daw::SettingsSnapshot& snapshot, bool allowBlocking = false);
    // Preparation and the optional durable-write callback precede publication.
    // Missing hardware after publication uses ordinary endpoint recovery.
    [[nodiscard]] juce::Result applySettingsSnapshot(
        const mic_daw::SettingsSnapshot& snapshot,
        const std::function<juce::Result()>& beforeCommit = {});
    [[nodiscard]] std::uint64_t getPluginChangeRevision() const noexcept
    {
        return pluginChangeRevision.load(std::memory_order_acquire);
    }

    [[nodiscard]] EndpointSnapshot getInputSnapshot() const;
    [[nodiscard]] EndpointSnapshot getOutputSnapshot() const;
    // One UI consumer: peak values consume maxima since the previous snapshot.
    [[nodiscard]] MeterSnapshot getMeterSnapshot() const noexcept;
    [[nodiscard]] juce::StringArray getRecentLog() const;

    void restoreSettings(juce::PropertiesFile& settings);
    // Call on the message thread. Metadata and each slot's last valid state are
    // always written; failure means one or more current snapshots need a retry.
    // Blocking snapshots are permitted only after the bridge has been stopped.
    [[nodiscard]] juce::Result saveSettings(juce::PropertiesFile& settings,
                                           bool allowBlocking = false);

private:
    class SharedWasapiDeviceManager final : public juce::AudioDeviceManager
    {
    protected:
        void createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) override;
    };

    class StereoRingBuffer final
    {
    public:
        explicit StereoRingBuffer(std::size_t capacityFrames);

        int push(const float* left, const float* right, int frames) noexcept;
        [[nodiscard]] std::size_t available() const noexcept;
        [[nodiscard]] float peek(int channel, std::size_t offset) const noexcept;
        void consume(std::size_t frames) noexcept;
        void requestClear() noexcept;
        bool applyPendingClear() noexcept;
        [[nodiscard]] std::size_t capacity() const noexcept { return capacityFrames; }
        [[nodiscard]] std::uint64_t getOverrunCount() const noexcept;

    private:
        const std::size_t capacityFrames;
        std::array<std::vector<float>, 2> channels;
        std::atomic<std::uint64_t> readPosition { 0 };
        std::atomic<std::uint64_t> writePosition { 0 };
        std::atomic<bool> clearRequested { false };
        std::atomic<std::uint64_t> overruns { 0 };
    };

    class InputCallback final : public juce::AudioIODeviceCallback
    {
    public:
        explicit InputCallback(AudioEngine& ownerToUse) : owner(ownerToUse) {}

        void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                              int numInputChannels,
                                              float* const* outputChannelData,
                                              int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override;
        void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
        void audioDeviceStopped() override;
        void audioDeviceError(const juce::String&) override;

    private:
        AudioEngine& owner;
        mic_daw::RollingRmsMeter rmsMeter;
        int meterChannelMode = -1;
    };

    class OutputCallback final : public juce::AudioIODeviceCallback
    {
    public:
        explicit OutputCallback(AudioEngine& ownerToUse) : owner(ownerToUse) {}

        void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                              int numInputChannels,
                                              float* const* outputChannelData,
                                              int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override;
        void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
        void audioDeviceStopped() override;
        void audioDeviceError(const juce::String&) override;

    private:
        AudioEngine& owner;
        juce::AudioBuffer<float> processingBuffer;
        mic_daw::RollingRmsMeter rmsMeter;
    };

    class PluginRuntime final : private juce::AudioProcessorListener
    {
    public:
        explicit PluginRuntime(std::atomic<std::uint64_t>& revision) : changeRevision(revision) {}
        ~PluginRuntime() { clear(); }
        juce::Result install(std::unique_ptr<juce::AudioPluginInstance> newPlugin);
        void clear();
        void prepare(double sampleRate, int maximumBlockSize);
        void release();
        void process(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;

        void setBypassed(bool shouldBypass) noexcept
        {
            bypassed.store(shouldBypass, std::memory_order_release);
            if (!shouldBypass)
                disabledAfterProcessFault.store(false, std::memory_order_release);
        }
        [[nodiscard]] bool isBypassed() const noexcept { return bypassed.load(); }
        [[nodiscard]] juce::AudioPluginInstance* get() const noexcept { return plugin.get(); }
        [[nodiscard]] bool hasPlugin() const noexcept { return plugin != nullptr; }
        [[nodiscard]] bool consumeFault() noexcept { return faulted.exchange(false); }
        [[nodiscard]] bool hasLifecycleFault() const noexcept
        {
            return lifecycleFaulted.load(std::memory_order_acquire);
        }
        [[nodiscard]] bool hasExpectedLayout() const;
        [[nodiscard]] int getReportedLatencySamples() const noexcept
        {
            return reportedLatencySamples.load(std::memory_order_relaxed);
        }

    private:
        void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float) override
        {
            if (juce::isPositiveAndBelow(index, static_cast<int>(parameterAffectsState.size()))
                && parameterAffectsState[static_cast<std::size_t>(index)] == 0)
                return;
            changeRevision.fetch_add(1, std::memory_order_release);
        }
        void audioProcessorChanged(juce::AudioProcessor*,
                                   const juce::AudioProcessorListener::ChangeDetails& details) override
        {
            if (details.programChanged || details.nonParameterStateChanged)
                changeRevision.fetch_add(1, std::memory_order_release);
        }
        std::atomic<std::uint64_t>& changeRevision;
        // Constructed before listener registration, never changed while live.
        std::vector<unsigned char> parameterAffectsState;
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> dryBuffer;
        std::atomic<bool> bypassed { false };
        std::atomic<bool> faulted { false };
        std::atomic<bool> disabledAfterProcessFault { false };
        std::atomic<bool> lifecycleFaulted { false };
        std::atomic<int> reportedLatencySamples { 0 };
        std::atomic<bool> processReady { false };
        bool releasePending = false;
        int inputChannels = 2;
        int outputChannels = 2;
    };

    struct PluginSlot
    {
        explicit PluginSlot(std::atomic<std::uint64_t>& revision) : runtime(revision) {}
        PluginRuntime runtime;
        juce::String name;
        juce::String path;
        juce::String identifier;
        juce::String formatName;
        // Moves with this slot, so deferred snapshots never use another
        // plugin's old state after a reorder, deletion, or replacement.
        juce::String savedStateBase64;
        juce::String restoreError;
    };

    struct EndpointControl
    {
        juce::String desiredDevice;
        juce::String lastError;
        int retryCount = 0;
        mic_daw::RecoveryState recovery;
        std::int64_t lastObservedCallbackMs = 0;

        std::atomic<bool> callbackRunning { false };
        std::atomic<bool> callbackError { false };
        std::atomic<bool> expectedDeviceConfigured { false };
        std::atomic<std::int64_t> expectedDeviceHash { 0 };
        std::atomic<std::int64_t> callbackDeviceHash { 0 };
        std::atomic<std::int64_t> lastCallbackMs { 0 };
        std::atomic<double> sampleRate { 0.0 };
        std::atomic<int> bufferSize { 0 };

        void armCallbackFor(const juce::String& deviceName) noexcept
        {
            expectedDeviceHash.store(static_cast<std::int64_t>(deviceName.hashCode64()),
                                     std::memory_order_relaxed);
            expectedDeviceConfigured.store(deviceName.isNotEmpty(),
                                           std::memory_order_release);
        }

        void disarmCallback() noexcept
        {
            expectedDeviceConfigured.store(false, std::memory_order_release);
        }

        void setCallbackDevice(const juce::String& deviceName) noexcept
        {
            callbackDeviceHash.store(static_cast<std::int64_t>(deviceName.hashCode64()),
                                     std::memory_order_release);
        }

        void clearCallbackDevice() noexcept
        {
            callbackDeviceHash.store(0, std::memory_order_release);
        }

        [[nodiscard]] bool callbackIsAuthorized() const noexcept
        {
            if (!expectedDeviceConfigured.load(std::memory_order_acquire))
                return false;

            return callbackDeviceHash.load(std::memory_order_acquire)
                   == expectedDeviceHash.load(std::memory_order_relaxed);
        }
    };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void initialiseDeviceManagers();
    [[nodiscard]] juce::Result loadPluginInternal(int slotIndex,
                                                 const juce::File& module,
                                                 const juce::String& preferredIdentifier,
                                                 const juce::String& preferredFormatName,
                                                 const juce::String& stateBase64,
                                                 bool bypassed,
                                                 bool restoring);
    [[nodiscard]] juce::Result createPluginSlot(
        const juce::File& module,
        const juce::String& preferredIdentifier,
        const juce::String& preferredFormatName,
        const juce::String& stateBase64,
        bool bypassed,
        std::unique_ptr<PluginSlot>& createdSlot);
    void detachInputCallback();
    void attachInputCallback();
    void detachOutputCallback();
    void attachOutputCallback();
    void preparePluginChain(double sampleRate, int maximumBlockSize);
    void releasePluginChain();
    void attemptInputOpen();
    void attemptOutputOpen();
    void scheduleFailure(EndpointControl& endpoint, const juce::String& error, bool closeDevice);
    void requestImmediateRetry(EndpointControl& endpoint);
    void checkEndpointHealth(EndpointControl& endpoint, bool isInput, std::int64_t nowMs);
    [[nodiscard]] bool bridgeMayUseDevices() const noexcept
    {
        return bridgeEnabled;
    }
    [[nodiscard]] static EndpointState toPublicState(const EndpointControl& endpoint,
                                                     bool bridgeEnabled) noexcept;

    void renderInput(juce::AudioBuffer<float>& destination, int numSamples) noexcept;
    void processOutput(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;
    void addLog(const juce::String& line);
    void notifyStateChanged();
    [[nodiscard]] juce::AudioIODeviceType* getWasapiType(SharedWasapiDeviceManager& manager) const;
    [[nodiscard]] static std::int64_t nowMs() noexcept;

    SharedWasapiDeviceManager inputManager;
    SharedWasapiDeviceManager outputManager;
    InputCallback inputCallback { *this };
    OutputCallback outputCallback { *this };
    bool inputCallbackRegistered = false;
    bool outputCallbackRegistered = false;

    StereoRingBuffer ring { 131072 };
    // Declared before the slots so listeners never outlive their counter.
    std::atomic<std::uint64_t> pluginChangeRevision { 0 };
    std::vector<std::unique_ptr<PluginSlot>> pluginSlots;
    juce::AudioPluginFormatManager pluginFormats;

    EndpointControl inputEndpoint;
    EndpointControl outputEndpoint;
    juce::StringArray inputDevices;
    juce::StringArray outputDevices;

    bool bridgeEnabled = false;
    bool autoRecover = true;
    bool deviceListDirty = true;
    std::int64_t nextDeviceScanMs = 0;
    std::atomic<int> inputChannelMode { 0 };

    double resamplePosition = 0.0;
    bool inputPrimed = false;
    float inputFadeGain = 0.0f;
    mic_daw::PeakAccumulator inputPeak;
    mic_daw::PeakAccumulator outputPeak;
    std::atomic<float> inputRms { 0.0f };
    std::atomic<float> outputRms { 0.0f };
    std::atomic<int> ringOccupancy { 0 };
    std::atomic<int> ringTarget { 0 };
    std::atomic<double> correctionPpm { 0.0 };
    std::atomic<std::uint64_t> underruns { 0 };
    std::atomic<std::uint64_t> hardResyncs { 0 };

    mutable juce::CriticalSection logLock;
    juce::StringArray recentLog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
