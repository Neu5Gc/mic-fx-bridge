#pragma once

#include <juce_core/juce_core.h>

#include "UiText.h"

#include <utility>

namespace mic_daw
{
// Keep this operation separate from the rack/device code so its failure paths
// can be exercised with a fake plugin, without opening an audio device. Plugin
// must provide getCallbackLock() and getStateInformation(juce::MemoryBlock&).
// The cache belongs to the plugin instance, not to its current rack index.
template <typename Plugin>
[[nodiscard]] juce::Result capturePluginState(Plugin& plugin,
                                             juce::String& savedStateBase64,
                                             bool allowBlocking)
{
    juce::MemoryBlock candidate;
    const auto capture = [&]() -> juce::Result
    {
        try
        {
            plugin.getStateInformation(candidate);

            // Some failing plugins leave the destination empty or partially
            // fill it before throwing. Neither may replace the last good blob.
            if (candidate.getSize() == 0)
                return juce::Result::fail(
                    uiText(u8"플러그인이 빈 설정 데이터를 반환해 이전 상태를 유지했습니다.",
                           "The plugin returned empty settings data, so the previous state was kept."));

            return juce::Result::ok();
        }
        catch (...)
        {
            return juce::Result::fail(
                uiText(u8"플러그인 설정을 읽는 중 예외가 발생해 이전 상태를 유지했습니다.",
                       "An exception occurred while reading the plugin settings, so the previous state was kept."));
        }
    };

    auto captureResult = juce::Result::ok();
    if (allowBlocking)
    {
        const juce::ScopedLock callbackLock(plugin.getCallbackLock());
        captureResult = capture();
    }
    else
    {
        // A live autosave must not wait for a currently executing process block.
        // getStateInformation itself is third-party code and may still be slow
        // after this lock is acquired; this is not a realtime-safety guarantee.
        const juce::ScopedTryLock callbackLock(plugin.getCallbackLock());
        if (!callbackLock.isLocked())
            return juce::Result::fail(
                uiText(u8"플러그인이 처리 중이어서 설정 읽기를 다음 저장으로 미뤘습니다.",
                       "The plugin is processing audio, so reading its settings was deferred until the next save."));

        captureResult = capture();
    }

    if (captureResult.failed())
        return captureResult;

    // Encoding is host work and does not touch the plugin. Keep it outside the
    // callback lock so the audio thread only waits for the third-party state read.
    try
    {
        auto encoded = candidate.toBase64Encoding();
        savedStateBase64 = std::move(encoded);
        return juce::Result::ok();
    }
    catch (...)
    {
        return juce::Result::fail(
            uiText(u8"플러그인 설정을 인코딩하지 못해 이전 상태를 유지했습니다.",
                   "The plugin settings could not be encoded, so the previous state was kept."));
    }
}
} // namespace mic_daw
