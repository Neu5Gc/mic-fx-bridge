#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "SettingsSnapshot.h"
#include "DebouncedSave.h"
#include "PluginBrowser.h"
#include "UiText.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

class MainComponent final : public juce::Component,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    [[nodiscard]] bool prepareToQuit();

private:
    class LevelMeter final : public juce::Component
    {
    public:
        void setLevel(float newLevel);
        void paint(juce::Graphics& graphics) override;

    private:
        float level = 0.0f;
    };

    class PluginEditorWindow final : public juce::DocumentWindow
    {
    public:
        PluginEditorWindow(juce::AudioPluginInstance& plugin,
                           std::function<void()> closeCallback);
        ~PluginEditorWindow() override;
        void closeButtonPressed() override;

    private:
        std::function<void()> onClose;
    };

    class FxSlotRow final : public juce::Component
    {
    public:
        explicit FxSlotRow(int slotIndexToUse);

        void paint(juce::Graphics& graphics) override;
        void resized() override;
        void update(const AudioEngine::PluginSlotSnapshot* snapshot,
                    int pluginCount,
                    bool controlsEnabled);

        std::function<void(int)> onLoad;
        std::function<void(int)> onEdit;
        std::function<void(int)> onRemove;
        std::function<void(int, int)> onMove;
        std::function<void(int, bool)> onBypass;

    private:
        const int slotIndex;
        juce::Label numberLabel;
        juce::Label nameLabel;
        juce::TextButton loadButton;
        juce::ToggleButton bypassButton { "Bypass" };
        juce::TextButton editorButton { mic_daw::uiText(u8"편집", "Edit") };
        juce::TextButton moveUpButton { juce::CharPointer_UTF8("\xe2\x86\x91") };
        juce::TextButton moveDownButton { juce::CharPointer_UTF8("\xe2\x86\x93") };
        juce::TextButton removeButton { mic_daw::uiText(u8"삭제", "Remove") };
        bool occupied = false;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void configureProperties();
    void configureControls();
    void chooseSettingsFile(bool saving);
    void confirmSettingsLoad(const juce::File& file);
    void finishSettingsFileOperation();
    [[nodiscard]] juce::Result preserveSettingsBeforeLoad();
    [[nodiscard]] juce::Result captureFullSettings(mic_daw::SettingsSnapshot& snapshot);
    [[nodiscard]] juce::Result writeCurrentSettings(const mic_daw::SettingsSnapshot& snapshot,
                                                   bool recovering = false);
    void settingsChanged();
    void observePluginChanges();
    juce::Result saveSettingsNow(bool manual = false, bool allowBlocking = false);
    [[nodiscard]] juce::Result captureAndSaveCurrentSettings(bool allowBlocking,
                                                            bool restartOnNextLaunch);
    void updateSaveStatus(const juce::String& text, const juce::String& detail, bool failed);
    [[nodiscard]] bool protectSettingsBeforeSave();
    [[nodiscard]] juce::File getLastPluginDirectory() const;
    void rememberPluginDirectory(const juce::File& directory, bool addSearchRoot = false);
    void updateDeviceMenus();
    void updateFromEngine();
    void choosePlugin(int slotIndex);
    void choosePluginFile(int slotIndex);
    void loadPluginIntoSlot(int slotIndex, const juce::File& file);
    void closePluginBrowser();
    void showPluginEditor(int slotIndex);
    void closePluginEditor();
    void removePlugin(int slotIndex);
    void movePlugin(int slotIndex, int destinationIndex);
    void setPluginBypassed(int slotIndex, bool bypassed);
    void showError(const juce::String& title, const juce::String& message);

    [[nodiscard]] static juce::Colour colourForState(AudioEngine::EndpointState state);
    [[nodiscard]] static juce::String describeEndpoint(
        const AudioEngine::EndpointSnapshot& endpoint);

    AudioEngine engine;
    juce::ApplicationProperties appProperties;
    juce::PropertiesFile* settings = nullptr;
    juce::LookAndFeel_V4 lookAndFeel;

    juce::Label titleLabel;
    juce::ToggleButton bridgeToggle { mic_daw::uiText(u8"브리지 실행", "Run bridge") };
    juce::ToggleButton autoRecoverToggle { mic_daw::uiText(u8"자동 복구", "Auto-recover") };
    juce::TextButton retryButton { mic_daw::uiText(u8"오디오 다시 연결", "Reconnect audio") };
    juce::TextButton refreshButton { mic_daw::uiText(u8"장치 목록 갱신", "Refresh device list") };

    juce::GroupComponent inputGroup { "input", mic_daw::uiText(u8"WASAPI 마이크 입력", "WASAPI microphone input") };
    juce::Label inputDeviceLabel { {}, mic_daw::uiText(u8"입력 장치", "Device") };
    juce::ComboBox inputDeviceBox;
    juce::Label inputChannelLabel { {}, mic_daw::uiText(u8"채널", "Channel") };
    juce::ComboBox inputChannelBox;
    juce::Label inputStatusLabel;
    juce::Label inputMeterLabel { {}, "IN" };
    LevelMeter inputMeter;
    juce::Label inputRmsLabel { {}, juce::String(u8"PK −∞ dBFS\nRMS −∞ dBFS") };

    juce::GroupComponent outputGroup { "output", mic_daw::uiText(u8"가상 마이크 출력", "Virtual microphone output") };
    juce::Label outputDeviceLabel { {}, mic_daw::uiText(u8"출력 장치", "Device") };
    juce::ComboBox outputDeviceBox;
    juce::Label outputStatusLabel;
    juce::Label outputMeterLabel { {}, "OUT" };
    LevelMeter outputMeter;
    juce::Label outputRmsLabel { {}, juce::String(u8"PK −∞ dBFS\nRMS −∞ dBFS") };

    juce::GroupComponent pluginGroup { "plugin", mic_daw::uiText(u8"효과 체인", "Effects chain") };
    juce::TextButton browsePluginsButton { mic_daw::uiText(u8"VST 탐색", "Browse VSTs") };
    juce::TextButton saveButton { mic_daw::uiText(u8"설정 저장…", "Save settings...") };
    juce::TextButton loadButton { mic_daw::uiText(u8"설정 불러오기…", "Load settings...") };
    juce::Label saveStatusLabel;
    juce::Component rackContent;
    juce::Viewport rackViewport;
    std::array<std::unique_ptr<FxSlotRow>, AudioEngine::kMaxPluginSlots> fxRows;

    juce::GroupComponent diagnosticsGroup { "diagnostics", mic_daw::uiText(u8"상태 및 로그", "Status and log") };
    juce::Label driftLabel;
    juce::TextEditor logView;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<PluginBrowserWindow> pluginBrowserWindow;
    std::unique_ptr<PluginEditorWindow> pluginEditorWindow;
    juce::AudioPluginInstance* editedPlugin = nullptr;
    int editedPluginSlot = -1;
    std::uint64_t pluginEditorGeneration = 0;
    std::uint64_t pluginBrowserGeneration = 0;
    std::uint64_t settingsFileGeneration = 0;
    std::uint64_t observedPluginRevision = 0;
    juce::String lastPluginDirectory;
    juce::StringArray pluginSearchRoots;
    juce::String persistenceBlockedReason;
    juce::String startupDiagnostics;
    mic_daw::DebouncedSave autosave;
    int expectedStartupPluginCount = 0;
    bool startupSettingsProtected = false;
    bool shuttingDown = false;
    bool shutdownSnapshotCommitted = false;
    bool chooserBusy = false;
    bool settingsFileBusy = false;
    bool updatingControls = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
