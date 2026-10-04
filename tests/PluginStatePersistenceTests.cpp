#include "PluginStatePersistence.h"

#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
int checks = 0;

void require(bool condition, const char* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

struct FakePlugin
{
    enum class Behaviour { valid, empty, throwImmediately, writeThenThrow };

    const juce::CriticalSection& getCallbackLock() const noexcept { return callbackLock; }

    void getStateInformation(juce::MemoryBlock& destination)
    {
        ++snapshotCalls;
        if (onSnapshot)
            onSnapshot();

        switch (behaviour)
        {
            case Behaviour::valid:
                destination = payload;
                return;
            case Behaviour::empty:
                return;
            case Behaviour::throwImmediately:
                throw std::runtime_error("Fake state exception");
            case Behaviour::writeThenThrow:
                destination = payload;
                throw std::runtime_error("Fake exception after writing partial state");
        }
    }

    juce::CriticalSection callbackLock;
    juce::MemoryBlock payload;
    Behaviour behaviour = Behaviour::valid;
    int snapshotCalls = 0;
    std::function<void()> onSnapshot;
};

juce::MemoryBlock binaryState()
{
    constexpr std::array<unsigned char, 8> bytes { 0, 255, 1, 128, 2, 0, 192, 42 };
    return { bytes.data(), bytes.size() };
}

bool anotherThreadCanLock(const juce::CriticalSection& lock)
{
    bool wasLocked = false;
    std::thread observer([&]
    {
        const juce::ScopedTryLock probe(lock);
        wasLocked = probe.isLocked();
    });
    observer.join();
    return wasLocked;
}

void testSuccessfulSnapshot(bool allowBlocking)
{
    FakePlugin plugin;
    plugin.payload = binaryState();
    bool callbackLockHeld = false;
    plugin.onSnapshot = [&]
    {
        callbackLockHeld = !anotherThreadCanLock(plugin.getCallbackLock());
    };

    juce::String cache = "previous-good-state";
    const auto result = mic_daw::capturePluginState(plugin, cache, allowBlocking);
    require(result.wasOk(), "A valid plugin snapshot must succeed");
    require(plugin.snapshotCalls == 1, "A successful snapshot must read state once");
    require(callbackLockHeld, "Both snapshot modes must hold the plugin callback lock");
    require(anotherThreadCanLock(plugin.getCallbackLock()), "Successful snapshot leaked its lock");
    require(cache == plugin.payload.toBase64Encoding(), "Snapshot must store the current state");

    juce::MemoryBlock restored;
    require(restored.fromBase64Encoding(cache), "Saved snapshot must be valid base64");
    require(restored == plugin.payload, "Binary bytes must survive the snapshot roundtrip");
}

void testFailedSnapshot(FakePlugin::Behaviour behaviour, bool allowBlocking)
{
    FakePlugin plugin;
    plugin.payload = binaryState();
    plugin.behaviour = behaviour;
    const auto restoredState = plugin.payload.toBase64Encoding();
    juce::String cache = restoredState;

    const auto result = mic_daw::capturePluginState(plugin, cache, allowBlocking);
    require(result.failed(), "Failed or empty plugin state must report a retryable failure");
    require(result.getErrorMessage().isNotEmpty(), "Snapshot failure must explain the problem");
    require(cache == restoredState, "A failed snapshot must preserve the known-good/restored state");
    require(anotherThreadCanLock(plugin.getCallbackLock()), "Failed snapshot leaked its lock");

    // A new/replacement slot owns a fresh cache; it must not fabricate a state
    // after failure or accidentally inherit a blob from a different plugin.
    juce::String newSlotCache;
    const auto initial = mic_daw::capturePluginState(plugin, newSlotCache, allowBlocking);
    require(initial.failed(), "A failed initial snapshot must report failure");
    require(newSlotCache.isEmpty(), "Failed initial snapshot must not invent recovery data");

    plugin.behaviour = FakePlugin::Behaviour::valid;
    const auto retried = mic_daw::capturePluginState(plugin, cache, allowBlocking);
    require(retried.wasOk(), "A later valid snapshot must recover after failure");
    require(cache == plugin.payload.toBase64Encoding(), "Retry must commit the valid snapshot");
}

void testBusyCallbackDefersWithoutReading()
{
    FakePlugin plugin;
    plugin.payload = binaryState();
    juce::String cache = "saved-before-active-audio";
    juce::WaitableEvent lockHeld;
    juce::WaitableEvent releaseCallback;

    std::thread callback([&]
    {
        const juce::ScopedLock lock(plugin.getCallbackLock());
        lockHeld.signal();
        // The timeout bounds the test even if a regression replaces try-lock
        // with a blocking lock. No real audio thread or device is used.
        static_cast<void>(releaseCallback.wait(2000.0));
    });

    const auto started = lockHeld.wait(2000.0);
    const auto before = std::chrono::steady_clock::now();
    const auto result = mic_daw::capturePluginState(plugin, cache, false);
    const auto elapsed = std::chrono::steady_clock::now() - before;
    releaseCallback.signal();
    callback.join();

    require(started, "The fake audio callback did not start");
    require(result.failed(), "A live snapshot must defer while the callback lock is busy");
    require(elapsed < std::chrono::milliseconds(1000), "A live snapshot waited for active processing");
    require(plugin.snapshotCalls == 0, "A deferred snapshot must not enter the plugin");
    require(cache == "saved-before-active-audio", "A deferred snapshot must preserve the old cache");

    const auto retried = mic_daw::capturePluginState(plugin, cache, false);
    require(retried.wasOk(), "The next autosave must succeed after the callback lock is released");
    require(plugin.snapshotCalls == 1, "The retry must snapshot exactly once");
    require(cache == plugin.payload.toBase64Encoding(), "Retry must update the cache after contention");
}
} // namespace

int main()
{
    try
    {
        for (const auto allowBlocking : { false, true })
        {
            testSuccessfulSnapshot(allowBlocking);
            for (const auto behaviour : { FakePlugin::Behaviour::empty,
                                          FakePlugin::Behaviour::throwImmediately,
                                          FakePlugin::Behaviour::writeThenThrow })
                testFailedSnapshot(behaviour, allowBlocking);
        }

        testBusyCallbackDefersWithoutReading();
        std::cout << "Plugin state persistence: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Plugin state persistence failed: " << error.what() << '\n';
        return 1;
    }
}
