#include "MainComponent.h"
#include "MeterReadout.h"
#include "SettingsDiskStore.h"
#include "UiLayout.h"

#include <cmath>
#include <initializer_list>

namespace
{
constexpr auto kMargin = 20;
constexpr auto kGap = 12;
constexpr auto kRowHeight = 30;
constexpr auto kFxRowHeight = 34;
constexpr auto kFxRowGap = 4;
constexpr auto kMinDiagnosticsHeight = 150;
constexpr auto kRmsLabelWidth = 142;

void styleSectionLabel(juce::Label& label)
{
    label.setColour(juce::Label::textColourId, juce::Colour(0xffaab6c8));
    label.setJustificationType(juce::Justification::centredLeft);
}
} // namespace

//==============================================================================
void MainComponent::LevelMeter::setLevel(float newLevel)
{
    level = juce::jlimit(0.0f, 1.5f, newLevel);
    repaint();
}

void MainComponent::LevelMeter::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(juce::Colour(0xff111722));
    graphics.fillRoundedRectangle(bounds, 4.0f);

    const auto decibels = juce::Decibels::gainToDecibels(level, -60.0f);
    const auto proportion = juce::jlimit(0.0f, 1.0f, (decibels + 60.0f) / 60.0f);
    auto filled = bounds.reduced(2.0f);
    filled.setWidth(filled.getWidth() * proportion);

    juce::ColourGradient gradient(juce::Colour(0xff33d17a), filled.getX(), 0.0f,
                                  juce::Colour(0xffff5c5c), bounds.getRight(), 0.0f, false);
    gradient.addColour(0.72, juce::Colour(0xffffc857));
    graphics.setGradientFill(gradient);
    graphics.fillRoundedRectangle(filled, 3.0f);

    graphics.setColour(juce::Colour(0xff3c4657));
    graphics.drawRoundedRectangle(bounds, 4.0f, 1.0f);
}

//==============================================================================
MainComponent::FxSlotRow::FxSlotRow(int slotIndexToUse)
    : slotIndex(slotIndexToUse),
      loadButton(mic_daw::uiText(u8"추가", "Add"))
{
    numberLabel.setText("#" + juce::String(slotIndex + 1), juce::dontSendNotification);
    numberLabel.setJustificationType(juce::Justification::centred);
    numberLabel.setColour(juce::Label::textColourId, juce::Colour(0xff718198));

    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setColour(juce::Label::textColourId, juce::Colour(0xffdce5f2));
    nameLabel.setFont(juce::Font(juce::FontOptions(13.5f)));

    loadButton.setTooltip(mic_daw::uiText(
        u8"이 슬롯에 플러그인을 추가하거나 교체합니다.",
        "Add or replace the plugin in this slot."));
    bypassButton.setTooltip(mic_daw::uiText(
        u8"이 플러그인만 우회합니다.",
        "Bypass only this plugin."));
    editorButton.setTooltip(mic_daw::uiText(
        u8"플러그인 편집기를 엽니다.",
        "Open the plugin editor."));
    moveUpButton.setTooltip(mic_daw::uiText(
        u8"한 단계 위로 이동합니다.",
        "Move up one position."));
    moveDownButton.setTooltip(mic_daw::uiText(
        u8"한 단계 아래로 이동합니다.",
        "Move down one position."));
    removeButton.setTooltip(mic_daw::uiText(
        u8"체인에서 제거합니다.",
        "Remove this plugin from the chain."));

    const std::array<juce::Component*, 8> components {
        &numberLabel, &nameLabel, &loadButton, &bypassButton,
        &editorButton, &moveUpButton, &moveDownButton, &removeButton
    };

    for (auto* component : components)
        addAndMakeVisible(component);

    loadButton.onClick = [this]
    {
        if (onLoad)
            onLoad(slotIndex);
    };
    editorButton.onClick = [this]
    {
        if (onEdit)
            onEdit(slotIndex);
    };
    removeButton.onClick = [this]
    {
        if (onRemove)
            onRemove(slotIndex);
    };
    moveUpButton.onClick = [this]
    {
        if (onMove)
            onMove(slotIndex, slotIndex - 1);
    };
    moveDownButton.onClick = [this]
    {
        if (onMove)
            onMove(slotIndex, slotIndex + 1);
    };
    bypassButton.onClick = [this]
    {
        if (onBypass)
            onBypass(slotIndex, bypassButton.getToggleState());
    };
}

void MainComponent::FxSlotRow::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(occupied ? juce::Colour(0xff182332) : juce::Colour(0xff111923));
    graphics.fillRoundedRectangle(bounds, 4.0f);
    graphics.setColour(juce::Colour(occupied ? 0xff34465f : 0xff253143));
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);
}

void MainComponent::FxSlotRow::resized()
{
    auto area = getLocalBounds().reduced(6, 2);
    numberLabel.setBounds(area.removeFromLeft(30));
    area.removeFromLeft(4);

    removeButton.setBounds(area.removeFromRight(50));
    area.removeFromRight(4);
    moveDownButton.setBounds(area.removeFromRight(32));
    area.removeFromRight(4);
    moveUpButton.setBounds(area.removeFromRight(32));
    area.removeFromRight(4);
    editorButton.setBounds(area.removeFromRight(54));
    area.removeFromRight(4);
    bypassButton.setBounds(area.removeFromRight(76));
    area.removeFromRight(4);
    loadButton.setBounds(area.removeFromRight(64));
    area.removeFromRight(8);
    nameLabel.setBounds(area);
}

void MainComponent::FxSlotRow::update(const AudioEngine::PluginSlotSnapshot* snapshot,
                                      int pluginCount,
                                      bool controlsEnabled)
{
    occupied = snapshot != nullptr;

    if (snapshot != nullptr)
    {
        auto description = snapshot->name;
        if (snapshot->formatName.isNotEmpty())
            description += juce::String(u8"  ·  ") + snapshot->formatName;
        if (!snapshot->available)
            description += mic_daw::uiText(u8"  ·  복원 대기", "  |  Waiting to restore");
        if (snapshot->reportedLatencySamples > 0)
            description += juce::String(u8"  ·  ") + juce::String(snapshot->reportedLatencySamples) + " samples";

        nameLabel.setText(description, juce::dontSendNotification);
        nameLabel.setTooltip(snapshot->restoreError.isNotEmpty()
                                 ? snapshot->path + "\n" + snapshot->restoreError
                                 : snapshot->path);
        nameLabel.setColour(juce::Label::textColourId,
                            !snapshot->available ? juce::Colour(0xffffa35c)
                            : snapshot->bypassed ? juce::Colour(0xff7f8da1)
                                                 : juce::Colour(0xffdce5f2));
        loadButton.setButtonText(mic_daw::uiText(u8"교체", "Replace"));
        bypassButton.setToggleState(snapshot->bypassed, juce::dontSendNotification);
    }
    else
    {
        nameLabel.setText(mic_daw::uiText(u8"비어 있음", "Empty"), juce::dontSendNotification);
        nameLabel.setTooltip({});
        nameLabel.setColour(juce::Label::textColourId, juce::Colour(0xff66758a));
        loadButton.setButtonText(mic_daw::uiText(u8"추가", "Add"));
        bypassButton.setToggleState(false, juce::dontSendNotification);
    }

    const auto canAddHere = slotIndex == pluginCount
                            && pluginCount < AudioEngine::kMaxPluginSlots;
    loadButton.setEnabled(controlsEnabled && (occupied || canAddHere));
    const auto available = snapshot != nullptr && snapshot->available;
    bypassButton.setEnabled(controlsEnabled && available);
    editorButton.setEnabled(controlsEnabled && available);
    removeButton.setEnabled(controlsEnabled && occupied);
    moveUpButton.setEnabled(controlsEnabled && occupied && slotIndex > 0);
    moveDownButton.setEnabled(controlsEnabled && occupied && slotIndex + 1 < pluginCount);
    repaint();
}

//==============================================================================
MainComponent::PluginEditorWindow::PluginEditorWindow(
    juce::AudioPluginInstance& plugin,
    std::function<void()> closeCallback)
    : juce::DocumentWindow(plugin.getName(),
                           juce::Colour(0xff171d27),
                           juce::DocumentWindow::minimiseButton
                               | juce::DocumentWindow::closeButton),
      onClose(std::move(closeCallback))
{
    juce::AudioProcessorEditor* editor = nullptr;
    if (plugin.hasEditor())
        editor = plugin.createEditorAndMakeActive();
    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor(plugin);

    setUsingNativeTitleBar(true);
    setContentOwned(editor, true);
    setResizable(editor->isResizable(), false);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
    toFront(true);
}

MainComponent::PluginEditorWindow::~PluginEditorWindow()
{
    clearContentComponent();
}

void MainComponent::PluginEditorWindow::closeButtonPressed()
{
    setVisible(false);
    auto callback = onClose;
    juce::MessageManager::callAsync([callback = std::move(callback)]
    {
        if (callback)
            callback();
    });
}

//==============================================================================
MainComponent::MainComponent()
{
    configureProperties();
    configureControls();
    setLookAndFeel(&lookAndFeel);
    engine.addChangeListener(this);

    if (settings != nullptr && persistenceBlockedReason.isEmpty())
    {
        static_cast<void>(protectSettingsBeforeSave());
        engine.restoreSettings(*settings);
        const auto restored = engine.getPluginSlots();
        auto availableCount = 0;
        for (const auto& slot : restored)
            availableCount += slot.available ? 1 : 0;

        startupDiagnostics += mic_daw::uiText(u8"\n시작 복원: 저장 ", "\nStartup restore: stored ")
            + juce::String(expectedStartupPluginCount)
            + mic_daw::uiText(u8"개 / 체인 ", " / chain ")
            + juce::String(static_cast<int>(restored.size()))
            + mic_daw::uiText(u8"개 / 사용 가능 ", " / available ")
            + juce::String(availableCount) + mic_daw::uiText(u8"개", "");
        if (static_cast<int>(restored.size()) != expectedStartupPluginCount)
        {
            persistenceBlockedReason = mic_daw::uiText(
                u8"저장된 효과 수와 복원된 체인 수가 달라 기존 설정 덮어쓰기를 차단했습니다.",
                "The stored effect count does not match the restored chain. Existing settings will not be overwritten.");
            engine.setBridgeEnabled(false);
        }
        else if (startupSettingsProtected)
        {
            updateSaveStatus(
                juce::String(expectedStartupPluginCount)
                    + mic_daw::uiText(u8"개 복원 · 자동 저장 준비",
                                      " restored | autosave ready"),
                startupDiagnostics, false);
        }
    }

    if (persistenceBlockedReason.isNotEmpty())
        updateSaveStatus(mic_daw::uiText(u8"저장 차단 · 기존 설정 보호 중",
                                          "Save blocked | protecting existing settings"),
                         persistenceBlockedReason + "\n" + startupDiagnostics, true);

    updateDeviceMenus();
    updateFromEngine();
    autosave.saved();
    observedPluginRevision = engine.getPluginChangeRevision();
    startTimerHz(10);
    setSize(1000, 900);
}

MainComponent::~MainComponent()
{
    stopTimer();
    shuttingDown = true;


    ++pluginBrowserGeneration;
    ++settingsFileGeneration;
    pluginBrowserWindow.reset();
    fileChooser.reset();
    closePluginEditor();
    engine.removeChangeListener(this);
    const auto restartOnNextLaunch = engine.isBridgeEnabled();

    // Preserve a checkpoint before releaseResources as well as the final
    // stopped snapshot. A plugin that throws/returns empty retains its cache.
    if (!shutdownSnapshotCommitted)
        saveSettingsNow(false);
    engine.setBridgeEnabled(false);

    if (settings != nullptr)
    {
        if (!shutdownSnapshotCommitted && persistenceBlockedReason.isEmpty()
            && protectSettingsBeforeSave())
        {
            static_cast<void>(captureAndSaveCurrentSettings(true, restartOnNextLaunch));
        }

        // PropertiesFile's destructor otherwise retries even with its timer
        // disabled, bypassing our backup/restore guards after a failed save.
        settings->setNeedsToBeSaved(false);
    }

    setLookAndFeel(nullptr);
}

bool MainComponent::prepareToQuit()
{
    if (shutdownSnapshotCommitted)
        return true;




    // A failed startup read must still be closable without touching its file.
    if (settings == nullptr || persistenceBlockedReason.isNotEmpty())
        return true;
    if (!protectSettingsBeforeSave())
    {
        showError(mic_daw::uiText(u8"종료 전 저장에 실패했습니다",
                                    "Could not save before exit"),
                  saveStatusLabel.getTooltip());
        return false;
    }

    closePluginEditor();
    saveSettingsNow();
    const auto restartOnNextLaunch = engine.isBridgeEnabled();
    engine.setBridgeEnabled(false);
    const auto saved = captureAndSaveCurrentSettings(true, restartOnNextLaunch);
    if (saved.failed())
    {
        engine.setBridgeEnabled(restartOnNextLaunch);
        const auto detail = saved.getErrorMessage();
        updateSaveStatus(mic_daw::uiText(u8"종료 전 저장 실패 · 창 유지",
                                          "Pre-exit save failed | window kept open"),
                         detail, true);
        showError(mic_daw::uiText(u8"종료 전 저장에 실패했습니다",
                                    "Could not save before exit"),
                  detail + mic_daw::uiText(
                               u8"\n최근 설정을 잃지 않도록 창을 유지합니다. 저장 상태를 확인한 뒤 다시 종료하세요.",
                               "\nThe window will stay open to avoid losing recent settings. Check the save status, then exit again."));
        autosave.retry(juce::Time::getMillisecondCounterHiRes());
        return false;
    }

    shutdownSnapshotCommitted = true;
    shuttingDown = true;
    stopTimer();
    return true;
}

void MainComponent::configureProperties()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "MicVstBridge";
    options.filenameSuffix = "settings";
    options.folderName = "MicVstBridge";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    // A rack snapshot updates multiple keys. Only the guarded explicit commit
    // may write it, never a PropertiesFile timer between those updates.
    options.millisecondsBeforeSaving = -1;
    appProperties.setStorageParameters(options);
    settings = appProperties.getUserSettings();
    if (settings == nullptr)
    {
        persistenceBlockedReason = mic_daw::uiText(
            u8"사용자 설정 파일을 열지 못했습니다.",
            "Could not open the user settings file.");
        return;
    }

    startupDiagnostics = mic_daw::uiText(u8"설정 경로: ", "Settings path: ")
                         + settings->getFile().getFullPathName();
    const auto validation = mic_daw::validateStartupSettings(
        *settings, AudioEngine::kMaxPluginSlots, expectedStartupPluginCount);
    if (validation.failed())
        persistenceBlockedReason = validation.getErrorMessage();

    lastPluginDirectory = settings->getValue("lastPluginDirectory");
    pluginSearchRoots = juce::StringArray::fromLines(settings->getValue("vstSearchRoots"));
    pluginSearchRoots.trim();
    pluginSearchRoots.removeEmptyStrings();
    pluginSearchRoots.removeDuplicates(true);
}

bool MainComponent::protectSettingsBeforeSave()
{
    if (settings == nullptr || persistenceBlockedReason.isNotEmpty())
        return false;
    if (startupSettingsProtected)
        return true;

    const auto result = mic_daw::protectStartupSettings(*settings);
    startupSettingsProtected = result.wasOk();
    if (result.failed())
        updateSaveStatus(mic_daw::uiText(u8"저장 보류 · 시작 백업 실패",
                                          "Save deferred | startup backup failed"),
                         result.getErrorMessage(), true);
    return startupSettingsProtected;
}

void MainComponent::updateSaveStatus(const juce::String& text,
                                     const juce::String& detail,
                                     bool failed)
{
    saveStatusLabel.setText(text, juce::dontSendNotification);
    saveStatusLabel.setTooltip(detail);
    saveStatusLabel.setColour(juce::Label::textColourId,
                              juce::Colour(failed ? 0xffffa35c : 0xff90c9ab));
}

juce::Result MainComponent::saveSettingsNow(bool manual, bool allowBlocking)
{
    autosave.retry(juce::Time::getMillisecondCounterHiRes());
    if (settings == nullptr || persistenceBlockedReason.isNotEmpty()
        || !protectSettingsBeforeSave())
    {
        auto detail = persistenceBlockedReason.isNotEmpty()
                          ? persistenceBlockedReason
                          : saveStatusLabel.getTooltip();
        if (detail.isEmpty())
            detail = mic_daw::uiText(u8"설정 저장소를 사용할 수 없습니다.",
                                      "The settings store is unavailable.");
        if (manual)
            showError(mic_daw::uiText(u8"설정을 저장하지 못했습니다",
                                        "Could not save settings"),
                       detail);
        return juce::Result::fail(detail);
    }

    const auto saved = captureAndSaveCurrentSettings(allowBlocking, engine.isBridgeEnabled());
    if (saved.failed())
    {
        updateSaveStatus(mic_daw::uiText(u8"저장 실패 · 다시 시도 중",
                                          "Save failed | retrying"),
                         saved.getErrorMessage(), true);
        if (manual)
            showError(mic_daw::uiText(u8"설정을 저장하지 못했습니다",
                                        "Could not save settings"),
                      saved.getErrorMessage());
        return saved;
    }

    autosave.saved();
    updateSaveStatus(
        (manual ? mic_daw::uiText(u8"저장 완료 · ", "Saved | ")
                : mic_daw::uiText(u8"자동 저장 완료 · ", "Autosaved | "))
            + juce::String(static_cast<int>(engine.getPluginSlots().size()))
            + mic_daw::uiText(u8"개 · ", " effects | ")
            + juce::Time::getCurrentTime().formatted("%H:%M:%S"),
        settings->getFile().getFullPathName()
            + mic_daw::uiText(u8"\n이전 저장: .bak / 실행 시작 시 체인: .startup-backup",
                              "\nPrevious save: .bak / startup chain: .startup-backup"),
        false);
    return juce::Result::ok();
}

juce::Result MainComponent::captureAndSaveCurrentSettings(bool allowBlocking,
                                                         bool restartOnNextLaunch)
{
    juce::StringPairArray preferences(false);
    preferences.set("lastPluginDirectory", lastPluginDirectory);
    preferences.set("vstSearchRoots", pluginSearchRoots.joinIntoString("\n"));
    preferences.set("bridgeEnabled", restartOnNextLaunch ? "1" : "0");
    return mic_daw::captureAndSaveSettings(*settings, [&]
    {
        return engine.saveSettings(*settings, allowBlocking);
    }, preferences);
}

void MainComponent::settingsChanged()
{
    if (shuttingDown || persistenceBlockedReason.isNotEmpty())
        return;
    autosave.changed(juce::Time::getMillisecondCounterHiRes());
    updateSaveStatus(mic_daw::uiText(u8"변경됨 · 자동 저장 대기", "Changes pending | autosave in 5 seconds"),
                     mic_daw::uiText(u8"마지막 변경 후 5초 동안 추가 변경이 없으면 저장합니다.",
                                     "Saves after five seconds without another change."), false);
}

void MainComponent::observePluginChanges()
{
    const auto revision = engine.getPluginChangeRevision();
    if (revision != observedPluginRevision)
    {
        observedPluginRevision = revision;
        settingsChanged();
    }
}

juce::Result MainComponent::captureFullSettings(mic_daw::SettingsSnapshot& snapshot)
{
    if (settings == nullptr)
        return juce::Result::fail(mic_daw::uiText("The settings store is unavailable."));
    snapshot.properties = settings->getAllProperties();
    snapshot.properties.set("lastPluginDirectory", lastPluginDirectory);
    snapshot.properties.set("vstSearchRoots", pluginSearchRoots.joinIntoString("\n"));
    return engine.captureSettingsSnapshot(snapshot);
}

juce::Result MainComponent::writeCurrentSettings(const mic_daw::SettingsSnapshot& snapshot,
                                                 bool recovering)
{
    if (settings == nullptr)
        return juce::Result::fail(mic_daw::uiText("The settings store is unavailable."));
    if (!recovering && (persistenceBlockedReason.isNotEmpty() || !protectSettingsBeforeSave()))
        return juce::Result::fail(persistenceBlockedReason.isNotEmpty()
                                     ? persistenceBlockedReason : saveStatusLabel.getTooltip());
    // A verified full snapshot replaces the canonical file only after its previous
    // valid contents have been backed up. Invalid originals are preserved by the
    // explicit recovery copy made before an import, never rotated over a good .bak.
    const auto target = settings->getFile();
    if (target.existsAsFile())
    {
        juce::MemoryBlock previous;
        if (target.getSize() > 96 * 1024 * 1024 || !target.loadFileAsData(previous))
            return juce::Result::fail(mic_daw::uiText("Could not preserve the current settings file."));
        mic_daw::SettingsSnapshot previousSnapshot;
        if (mic_daw::SettingsSnapshotCodec::read(target, previousSnapshot).wasOk())
        {
            const auto backup = mic_daw::settings_disk_detail::writeBackupAtomically(
                target.getSiblingFile(target.getFileName() + ".bak"), previous);
            if (backup.failed())
                return backup;
        }
    }
    const auto written = mic_daw::SettingsSnapshotCodec::write(target, snapshot);
    if (written.failed())
        return written;
    const auto applied = mic_daw::SettingsSnapshotCodec::applyToProperties(snapshot, *settings);
    if (applied.failed())
        return applied;
    settings->setNeedsToBeSaved(false);
    return juce::Result::ok();
}

void MainComponent::finishSettingsFileOperation()
{
    settingsFileBusy = false;
    chooserBusy = false;
    if (!shuttingDown)
        updateFromEngine();
}

void MainComponent::chooseSettingsFile(bool saving)
{
    if (chooserBusy || shuttingDown || settings == nullptr)
        return;
    settingsFileBusy = true;
    chooserBusy = true;
    const auto generation = ++settingsFileGeneration;
    updateFromEngine();
    const auto initialFile = saving
        ? settings->getFile().getSiblingFile("Mic FX Bridge.settings")
        : settings->getFile().getParentDirectory();
    fileChooser = std::make_unique<juce::FileChooser>(
        saving ? mic_daw::uiText(u8"설정 저장", "Save settings")
               : mic_daw::uiText(u8"설정 불러오기", "Load settings"),
        initialFile, saving ? "*.settings" : "*.settings;*.bak;*.startup-backup");
    fileChooser->launchAsync(
        (saving ? juce::FileBrowserComponent::saveMode : juce::FileBrowserComponent::openMode)
            | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<MainComponent>(this), saving, generation]
        (const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || safeThis->settingsFileGeneration != generation)
                return;
            const auto selected = chooser.getResult();
            if (selected == juce::File{})
            {
                safeThis->finishSettingsFileOperation();
                return;
            }
            if (!saving)
            {
                safeThis->confirmSettingsLoad(selected);
                return;
            }
            const auto destination = selected.withFileExtension("settings");
            const auto save = [safeThis, generation, destination]
            {
                if (safeThis == nullptr || safeThis->settingsFileGeneration != generation)
                    return;
                safeThis->observePluginChanges();
                mic_daw::SettingsSnapshot snapshot;
                auto result = safeThis->captureFullSettings(snapshot);
                const auto canonical = safeThis->settings->getFile();
                bool exported = false;
                if (result.wasOk() && destination != canonical)
                {
                    result = mic_daw::SettingsSnapshotCodec::write(destination, snapshot);
                    exported = result.wasOk();
                }
                if (result.wasOk())
                    result = safeThis->writeCurrentSettings(snapshot);
                safeThis->finishSettingsFileOperation();
                if (result.failed())
                {
                    safeThis->autosave.retry(juce::Time::getMillisecondCounterHiRes());
                    const auto title = exported ? mic_daw::uiText("File saved | current settings save failed")
                                                : mic_daw::uiText("Could not save settings");
                    safeThis->updateSaveStatus(title, result.getErrorMessage(), true);
                    safeThis->showError(title, result.getErrorMessage());
                    return;
                }
                safeThis->autosave.saved();
                safeThis->updateSaveStatus(mic_daw::uiText("Saved | ") + destination.getFileName(),
                                           destination.getFullPathName(), false);
            };
            if (!destination.exists())
            {
                save();
                return;
            }
            juce::AlertWindow::showOkCancelBox(
                juce::MessageBoxIconType::QuestionIcon, mic_daw::uiText("Replace settings file?"),
                destination.getFullPathName(), mic_daw::uiText("Replace"), mic_daw::uiText("Cancel"), safeThis.getComponent(),
                juce::ModalCallbackFunction::create([safeThis, generation, save](int choice)
                {
                    if (safeThis == nullptr || safeThis->settingsFileGeneration != generation)
                        return;
                    if (choice == 1)
                        save();
                    else
                        safeThis->finishSettingsFileOperation();
                }));
        });
}

juce::Result MainComponent::preserveSettingsBeforeLoad()
{
    if (settings == nullptr)
        return juce::Result::fail(mic_daw::uiText("The settings store is unavailable."));
    const auto original = settings->getFile();
    const auto directory = original.getParentDirectory().getChildFile("settings-recovery")
        .getChildFile(juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S-")
                      + juce::Uuid().toString());
    const auto created = directory.createDirectory();
    if (created.failed())
        return created;
    for (const auto& suffix : { "", ".bak", ".startup-backup" })
    {
        const auto source = original.getSiblingFile(original.getFileName() + suffix);
        if (!source.existsAsFile())
            continue;
        const auto destination = directory.getChildFile(source.getFileName());
        juce::MemoryBlock before, after;
        if (source.getSize() > 96 * 1024 * 1024 || !source.loadFileAsData(before)
            || !destination.replaceWithData(before.getData(), before.getSize())
            || !destination.loadFileAsData(after) || before != after)
            return juce::Result::fail(mic_daw::uiText("Could not preserve the previous settings: ")
                                      + source.getFullPathName());
    }
    return juce::Result::ok();
}

void MainComponent::confirmSettingsLoad(const juce::File& file)
{
    mic_daw::SettingsSnapshot snapshot;
    const auto decoded = mic_daw::SettingsSnapshotCodec::read(file, snapshot);
    if (decoded.failed())
    {
        finishSettingsFileOperation();
        updateSaveStatus(mic_daw::uiText("Could not read settings"), decoded.getErrorMessage(), true);
        showError(mic_daw::uiText("Could not load settings"), decoded.getErrorMessage());
        return;
    }
    const auto generation = settingsFileGeneration;
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon, mic_daw::uiText("Load settings?"),
        file.getFileName() + mic_daw::uiText("\nReplace devices and the entire FX chain? "
            "These settings will also be used at the next launch. Previous files will be backed up."),
        mic_daw::uiText("Load"), mic_daw::uiText("Cancel"), this,
        juce::ModalCallbackFunction::create(
            [safeThis = juce::Component::SafePointer<MainComponent>(this), generation, file,
             snapshot = std::move(snapshot)](int choice)
            {
                if (safeThis == nullptr || safeThis->settingsFileGeneration != generation)
                    return;
                if (choice != 1)
                {
                    safeThis->finishSettingsFileOperation();
                    return;
                }
                // Destroy editors before their owning plugin instances can be retired.
                safeThis->closePluginEditor();
                auto result = safeThis->engine.applySettingsSnapshot(snapshot, [safeThis, &snapshot]
                {
                    const auto preserved = safeThis->preserveSettingsBeforeLoad();
                    return preserved.failed() ? preserved : safeThis->writeCurrentSettings(snapshot, true);
                });
                safeThis->finishSettingsFileOperation();
                if (result.failed())
                {
                    safeThis->updateSaveStatus(mic_daw::uiText("Load failed | current settings kept"), result.getErrorMessage(), true);
                    safeThis->showError(mic_daw::uiText("Could not load settings"), result.getErrorMessage());
                    return;
                }
                safeThis->persistenceBlockedReason.clear();
                safeThis->startupSettingsProtected = true;
                safeThis->lastPluginDirectory = safeThis->settings->getValue("lastPluginDirectory");
                safeThis->pluginSearchRoots = juce::StringArray::fromLines(
                    safeThis->settings->getValue("vstSearchRoots"));
                safeThis->pluginSearchRoots.trim();
                safeThis->pluginSearchRoots.removeEmptyStrings();
                safeThis->pluginSearchRoots.removeDuplicates(true);
                safeThis->observedPluginRevision = safeThis->engine.getPluginChangeRevision();
                safeThis->autosave.saved();
                safeThis->updateDeviceMenus();
                safeThis->updateFromEngine();
                safeThis->updateSaveStatus(mic_daw::uiText("Loaded | ") + file.getFileName(), file.getFullPathName(), false);
            }));
}

juce::File MainComponent::getLastPluginDirectory() const
{
    if (juce::File::isAbsolutePath(lastPluginDirectory))
    {
        const juce::File lastDirectory(lastPluginDirectory);
        if (lastDirectory.isDirectory())
            return lastDirectory;
    }

    const auto slots = engine.getPluginSlots();
    for (auto index = slots.size(); index > 0; --index)
        if (juce::File::isAbsolutePath(slots[index - 1].path))
        {
            const auto directory = juce::File(slots[index - 1].path).getParentDirectory();
            if (directory.isDirectory())
                return directory;
        }

    for (const auto& path : PluginBrowserWindow::defaultSearchRoots())
    {
        const juce::File directory(path);
        if (directory.isDirectory())
            return directory;
    }
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

void MainComponent::rememberPluginDirectory(const juce::File& directory, bool addSearchRoot)
{
    if (!directory.isDirectory())
        return;
    lastPluginDirectory = directory.getFullPathName();
    if (addSearchRoot && !pluginSearchRoots.contains(lastPluginDirectory, true))
        pluginSearchRoots.add(lastPluginDirectory);

    settingsChanged();
}

void MainComponent::configureControls()
{
    lookAndFeel.setDefaultSansSerifTypefaceName(
        mic_daw::uiText(u8"Malgun Gothic", "Segoe UI"));
    lookAndFeel.setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(0xff0c1119));
    lookAndFeel.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff192230));
    lookAndFeel.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff354258));
    lookAndFeel.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff27364a));
    lookAndFeel.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2f80ed));
    lookAndFeel.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffe8edf5));
    lookAndFeel.setColour(juce::GroupComponent::outlineColourId, juce::Colour(0xff2b3546));
    lookAndFeel.setColour(juce::GroupComponent::textColourId, juce::Colour(0xff90a7c7));

    titleLabel.setText("Mic FX Bridge", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(28.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(0xfff3f6fb));

    inputDeviceLabel.setJustificationType(juce::Justification::centredLeft);
    inputChannelLabel.setJustificationType(juce::Justification::centredLeft);
    outputDeviceLabel.setJustificationType(juce::Justification::centredLeft);
    inputMeterLabel.setJustificationType(juce::Justification::centred);
    outputMeterLabel.setJustificationType(juce::Justification::centred);
    for (auto* label : { &inputRmsLabel, &outputRmsLabel })
    {
        label->setJustificationType(juce::Justification::centredRight);
        label->setColour(juce::Label::textColourId, juce::Colour(0xffc1d9ed));
        label->setFont(juce::Font(juce::FontOptions(11.0f)));
        label->setMinimumHorizontalScale(1.0f);
    }
    inputMeterLabel.setTooltip(mic_daw::uiText(
        u8"PK: 최대 샘플 피크 / RMS: 최근 약 300 ms의 평균 레벨. 단위: dBFS.",
        "PK: maximum sample peak / RMS: average level over about 300 ms. Units: dBFS."));
    outputMeterLabel.setTooltip(inputMeterLabel.getTooltip());
    inputRmsLabel.setTooltip(mic_daw::uiText(
        u8"선택한 입력 채널의 보정·효과 처리 전 레벨입니다.",
        "Selected input channel level before effects."));
    outputRmsLabel.setTooltip(mic_daw::uiText(
        u8"보정·효과 처리 후 출력 레벨입니다. Windows와 외부 앱의 음량은 포함하지 않습니다.",
        "Output level after effects. Windows and external-app volume are not included."));
    styleSectionLabel(inputStatusLabel);
    styleSectionLabel(outputStatusLabel);
    styleSectionLabel(driftLabel);
    styleSectionLabel(saveStatusLabel);
    saveStatusLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    browsePluginsButton.setTooltip(mic_daw::uiText(
        u8"설치 폴더에서 VST 파일을 찾고 이름이나 경로로 검색합니다.",
        "Find VST files in installation folders and search by name or path."));
    saveButton.setTooltip(mic_daw::uiText("Save devices and all FX settings to a file."));
    loadButton.setTooltip(mic_daw::uiText("Load all settings from a file and use them at the next launch."));
    retryButton.setTooltip(mic_daw::uiText("Close and reopen the selected input and output devices."));
    refreshButton.setTooltip(mic_daw::uiText("Update the available input and output device lists."));
    inputChannelBox.addItem(mic_daw::uiText(u8"입력 1 → mono", "Input 1 -> mono"), 1);
    inputChannelBox.addItem(mic_daw::uiText(u8"입력 2 → mono", "Input 2 -> mono"), 2);
    inputChannelBox.addItem(mic_daw::uiText(u8"입력 1/2 stereo", "Inputs 1/2 stereo"), 3);

    logView.setMultiLine(true);
    logView.setReadOnly(true);
    logView.setScrollbarsShown(true);
    logView.setCaretVisible(false);
    logView.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff111722));
    logView.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff2b3546));
    logView.setColour(juce::TextEditor::textColourId, juce::Colour(0xffb9c6d8));
    logView.setFont(juce::Font(juce::FontOptions(14.0f).withName(
        mic_daw::uiText(u8"Malgun Gothic", "Segoe UI"))));

    rackViewport.setViewedComponent(&rackContent, false);
    rackViewport.setScrollBarsShown(true, false, true, false);

    const std::initializer_list<juce::Component*> components {
        &titleLabel, &bridgeToggle, &autoRecoverToggle,
        &retryButton, &refreshButton,
        &inputGroup, &inputDeviceLabel, &inputDeviceBox, &inputChannelLabel,
        &inputChannelBox, &inputStatusLabel, &inputMeterLabel, &inputMeter, &inputRmsLabel,
        &outputGroup, &outputDeviceLabel, &outputDeviceBox, &outputStatusLabel,
        &outputMeterLabel, &outputMeter, &outputRmsLabel,
        &pluginGroup, &browsePluginsButton, &saveButton, &saveStatusLabel, &rackViewport,
        &loadButton,
        &diagnosticsGroup, &driftLabel, &logView
    };

    for (auto* component : components)
        addAndMakeVisible(component);

    for (int slotIndex = 0; slotIndex < AudioEngine::kMaxPluginSlots; ++slotIndex)
    {
        auto row = std::make_unique<FxSlotRow>(slotIndex);
        row->onLoad = [this](int index) { choosePlugin(index); };
        row->onEdit = [this](int index) { showPluginEditor(index); };
        row->onRemove = [this](int index) { removePlugin(index); };
        row->onMove = [this](int index, int destination) { movePlugin(index, destination); };
        row->onBypass = [this](int index, bool bypassed)
        {
            setPluginBypassed(index, bypassed);
        };

        rackContent.addAndMakeVisible(row.get());
        fxRows[static_cast<std::size_t>(slotIndex)] = std::move(row);
    }

    bridgeToggle.onClick = [this]
    {
        if (!updatingControls)
        {
            engine.setBridgeEnabled(bridgeToggle.getToggleState());
            settingsChanged();
        }
    };
    autoRecoverToggle.onClick = [this]
    {
        if (!updatingControls)
        {
            engine.setAutoRecover(autoRecoverToggle.getToggleState());
            settingsChanged();
        }
    };
    inputDeviceBox.onChange = [this]
    {
        if (!updatingControls)
        {
            engine.setInputDevice(inputDeviceBox.getText());
            settingsChanged();
        }
    };
    outputDeviceBox.onChange = [this]
    {
        if (!updatingControls)
        {
            engine.setOutputDevice(outputDeviceBox.getText());
            settingsChanged();
        }
    };
    inputChannelBox.onChange = [this]
    {
        if (!updatingControls)
        {
            engine.setInputChannelMode(inputChannelBox.getSelectedId() - 1);
            settingsChanged();
        }
    };
    retryButton.onClick = [this] { engine.retryNow(); };
    refreshButton.onClick = [this]
    {
        engine.refreshDeviceLists();
        updateDeviceMenus();
    };
    browsePluginsButton.onClick = [this]
    {
        choosePlugin(static_cast<int>(engine.getPluginSlots().size()));
    };
    saveButton.onClick = [this] { chooseSettingsFile(true); };
    loadButton.onClick = [this] { chooseSettingsFile(false); };
}

void MainComponent::paint(juce::Graphics& graphics)
{
    juce::ColourGradient background(juce::Colour(0xff111a27), 0.0f, 0.0f,
                                    juce::Colour(0xff080c12), 0.0f,
                                    static_cast<float>(getHeight()), false);
    graphics.setGradientFill(background);
    graphics.fillAll();

    graphics.setColour(juce::Colour(0x202f80ed));
    graphics.fillEllipse(static_cast<float>(getWidth() - 260), -120.0f, 380.0f, 280.0f);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(kMargin);
    auto header = area.removeFromTop(42);
    titleLabel.setBounds(header.removeFromTop(36));

    auto primaryControls = area.removeFromTop(mic_daw::standardButtonHeight);
    const auto placeButton = [](juce::Rectangle<int>& row, juce::Component& button)
    {
        button.setBounds(row.removeFromLeft(mic_daw::standardButtonWidth)
                             .withHeight(mic_daw::standardButtonHeight));
        row.removeFromLeft(8);
    };
    placeButton(primaryControls, bridgeToggle);
    placeButton(primaryControls, autoRecoverToggle);
    placeButton(primaryControls, retryButton);
    placeButton(primaryControls, refreshButton);
    area.removeFromTop(8);
    auto secondaryControls = area.removeFromTop(mic_daw::standardButtonHeight);
    placeButton(secondaryControls, saveButton);
    placeButton(secondaryControls, loadButton);
    saveStatusLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(kGap);

    auto endpointArea = area.removeFromTop(210);
    auto inputArea = endpointArea.removeFromLeft((endpointArea.getWidth() - kGap) / 2);
    endpointArea.removeFromLeft(kGap);
    auto outputArea = endpointArea;
    inputGroup.setBounds(inputArea);
    outputGroup.setBounds(outputArea);

    auto inputContent = inputArea.reduced(16, 25);
    auto row = inputContent.removeFromTop(kRowHeight);
    inputDeviceLabel.setBounds(row.removeFromLeft(76));
    inputDeviceBox.setBounds(row);
    inputContent.removeFromTop(8);
    row = inputContent.removeFromTop(kRowHeight);
    inputChannelLabel.setBounds(row.removeFromLeft(76));
    inputChannelBox.setBounds(row);
    inputContent.removeFromTop(10);
    inputStatusLabel.setBounds(inputContent.removeFromTop(38));
    row = inputContent.removeFromTop(28);
    inputMeterLabel.setBounds(row.removeFromLeft(38));
    inputRmsLabel.setBounds(row.removeFromRight(kRmsLabelWidth));
    row.removeFromRight(8);
    inputMeter.setBounds(row.reduced(0, 5));

    auto outputContent = outputArea.reduced(16, 25);
    row = outputContent.removeFromTop(kRowHeight);
    outputDeviceLabel.setBounds(row.removeFromLeft(76));
    outputDeviceBox.setBounds(row);
    outputContent.removeFromTop(10);
    outputStatusLabel.setBounds(outputContent.removeFromTop(38));
    row = outputContent.removeFromTop(28);
    outputMeterLabel.setBounds(row.removeFromLeft(38));
    outputRmsLabel.setBounds(row.removeFromRight(kRmsLabelWidth));
    row.removeFromRight(8);
    outputMeter.setBounds(row.reduced(0, 5));

    area.removeFromTop(kGap);
    const auto pluginHeight = juce::jlimit(
        220, 330, area.getHeight() - kGap - kMinDiagnosticsHeight);
    auto pluginArea = area.removeFromTop(pluginHeight);
    pluginGroup.setBounds(pluginArea);
    auto pluginContent = pluginArea.reduced(14, 26);
    auto pluginToolbar = pluginContent.removeFromTop(30);
    browsePluginsButton.setBounds(pluginToolbar.removeFromLeft(mic_daw::standardButtonWidth));
    pluginContent.removeFromTop(8);
    rackViewport.setBounds(pluginContent);

    const auto contentHeight = AudioEngine::kMaxPluginSlots * (kFxRowHeight + kFxRowGap)
                               - kFxRowGap;
    const auto contentWidth = juce::jmax(
        560, rackViewport.getWidth() - rackViewport.getScrollBarThickness());
    rackContent.setSize(contentWidth, contentHeight);

    auto rackArea = rackContent.getLocalBounds();
    for (auto& rowComponent : fxRows)
    {
        rowComponent->setBounds(rackArea.removeFromTop(kFxRowHeight));
        rackArea.removeFromTop(kFxRowGap);
    }

    area.removeFromTop(kGap);
    diagnosticsGroup.setBounds(area);
    auto diagnosticsContent = area.reduced(16, 26);
    driftLabel.setBounds(diagnosticsContent.removeFromTop(28));
    diagnosticsContent.removeFromTop(6);
    logView.setBounds(diagnosticsContent);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    updateDeviceMenus();
    updateFromEngine();
}

void MainComponent::timerCallback()
{
    const auto meters = engine.getMeterSnapshot();
    inputMeter.setLevel(meters.inputPeak);
    outputMeter.setLevel(meters.outputPeak);
    inputRmsLabel.setText(mic_daw::formatPeakAndRms(meters.inputPeak, meters.inputRms), juce::dontSendNotification);
    outputRmsLabel.setText(mic_daw::formatPeakAndRms(meters.outputPeak, meters.outputRms), juce::dontSendNotification);

    const auto target = std::max(1, meters.ringTargetFrames);
    driftLabel.setText(
        "FIFO " + juce::String(meters.ringFrames) + " / " + juce::String(target)
        + mic_daw::uiText(u8" frames   ·   보정 ", " frames   |   correction ")
        + juce::String(meters.driftCorrectionPpm, 1)
        + juce::String(u8" ppm   ·   underrun ") + juce::String(meters.underruns)
        + juce::String(u8"   ·   overflow frames ") + juce::String(meters.overruns)
        + juce::String(u8"   ·   resync ") + juce::String(meters.hardResyncs)
        + juce::String(u8"   ·   FX latency ") + juce::String(meters.pluginLatencySamples) + " samples",
        juce::dontSendNotification);

    const auto log = startupDiagnostics + "\n" + engine.getRecentLog().joinIntoString("\n");
    if (logView.getText() != log)
    {
        logView.setText(log, false);
        logView.moveCaretToEnd();
    }

    updateFromEngine();
    observePluginChanges();
    if (!shuttingDown && !settingsFileBusy
        && autosave.due(juce::Time::getMillisecondCounterHiRes()))
        saveSettingsNow();
}

void MainComponent::updateDeviceMenus()
{
    const juce::ScopedValueSetter<bool> guard(updatingControls, true);

    auto inputs = engine.getInputDevices();
    const auto desiredInput = engine.getDesiredInputDevice();
    if (desiredInput.isNotEmpty() && !inputs.contains(desiredInput))
        inputs.add(desiredInput);
    inputDeviceBox.clear(juce::dontSendNotification);
    inputDeviceBox.addItemList(inputs, 1);
    inputDeviceBox.setText(desiredInput, juce::dontSendNotification);

    auto outputs = engine.getOutputDevices();
    const auto desiredOutput = engine.getDesiredOutputDevice();
    if (desiredOutput.isNotEmpty() && !outputs.contains(desiredOutput))
        outputs.add(desiredOutput);
    outputDeviceBox.clear(juce::dontSendNotification);
    outputDeviceBox.addItemList(outputs, 1);
    outputDeviceBox.setText(desiredOutput, juce::dontSendNotification);
}

void MainComponent::updateFromEngine()
{
    const juce::ScopedValueSetter<bool> guard(updatingControls, true);
    const auto controlsEnabled = !settingsFileBusy
                                 && persistenceBlockedReason.isEmpty();
    bridgeToggle.setToggleState(engine.isBridgeEnabled(), juce::dontSendNotification);
    autoRecoverToggle.setToggleState(engine.isAutoRecoverEnabled(), juce::dontSendNotification);
    inputChannelBox.setSelectedId(engine.getInputChannelMode() + 1, juce::dontSendNotification);
    bridgeToggle.setEnabled(controlsEnabled);
    autoRecoverToggle.setEnabled(controlsEnabled);
    retryButton.setEnabled(controlsEnabled);
    refreshButton.setEnabled(controlsEnabled);
    inputDeviceBox.setEnabled(controlsEnabled);
    inputChannelBox.setEnabled(controlsEnabled);
    outputDeviceBox.setEnabled(controlsEnabled);
    const auto fileControlsEnabled = !chooserBusy && !shuttingDown;
    saveButton.setEnabled(fileControlsEnabled && persistenceBlockedReason.isEmpty());
    loadButton.setEnabled(fileControlsEnabled);
    const auto input = engine.getInputSnapshot();
    const auto output = engine.getOutputSnapshot();
    inputStatusLabel.setText(describeEndpoint(input), juce::dontSendNotification);
    outputStatusLabel.setText(describeEndpoint(output), juce::dontSendNotification);
    inputStatusLabel.setColour(juce::Label::textColourId, colourForState(input.state));
    outputStatusLabel.setColour(juce::Label::textColourId, colourForState(output.state));

    const auto plugins = engine.getPluginSlots();
    const auto pluginCount = juce::jmin(static_cast<int>(plugins.size()),
                                        AudioEngine::kMaxPluginSlots);
    browsePluginsButton.setEnabled(controlsEnabled && !chooserBusy
                                    && pluginCount < AudioEngine::kMaxPluginSlots);

    if (pluginEditorWindow != nullptr && editedPlugin != nullptr)
    {
        auto editorStillValid = false;
        for (int slotIndex = 0; slotIndex < pluginCount; ++slotIndex)
        {
            if (engine.getPlugin(slotIndex) == editedPlugin)
            {
                editedPluginSlot = slotIndex;
                editorStillValid = true;
                break;
            }
        }

        if (!editorStillValid)
            closePluginEditor();
    }

    for (int slotIndex = 0; slotIndex < AudioEngine::kMaxPluginSlots; ++slotIndex)
    {
        const auto* snapshot = slotIndex < pluginCount
                                   ? &plugins[static_cast<std::size_t>(slotIndex)]
                                   : nullptr;
        fxRows[static_cast<std::size_t>(slotIndex)]->update(
            snapshot, pluginCount, controlsEnabled && !chooserBusy);
    }
}

void MainComponent::choosePlugin(int slotIndex)
{
    if (chooserBusy || persistenceBlockedReason.isNotEmpty()
        || slotIndex < 0 || slotIndex >= AudioEngine::kMaxPluginSlots)
        return;

    const auto pluginCount = static_cast<int>(engine.getPluginSlots().size());
    if (slotIndex > pluginCount)
        return;

    closePluginEditor();
    chooserBusy = true;
    updateFromEngine();

    const auto generation = ++pluginBrowserGeneration;
    const auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    PluginBrowserWindow::Callbacks callbacks;
    callbacks.onPick = [safeThis, generation, slotIndex](const juce::File& file)
    {
        if (safeThis == nullptr || safeThis->pluginBrowserGeneration != generation)
            return;
        safeThis->closePluginBrowser();
        safeThis->loadPluginIntoSlot(slotIndex, file);
    };
    callbacks.onBrowseFile = [safeThis, generation, slotIndex]
    {
        if (safeThis == nullptr || safeThis->pluginBrowserGeneration != generation)
            return;
        safeThis->closePluginBrowser();
        safeThis->choosePluginFile(slotIndex);
    };
    callbacks.onFolderAdded = [safeThis, generation](const juce::File& folder)
    {
        if (safeThis != nullptr && safeThis->pluginBrowserGeneration == generation)
            safeThis->rememberPluginDirectory(folder, true);
    };
    callbacks.onClose = [safeThis, generation]
    {
        if (safeThis != nullptr && safeThis->pluginBrowserGeneration == generation)
            safeThis->closePluginBrowser();
    };

    pluginBrowserWindow = std::make_unique<PluginBrowserWindow>(
        getLastPluginDirectory(), pluginSearchRoots, std::move(callbacks), &lookAndFeel);
}

void MainComponent::closePluginBrowser()
{
    ++pluginBrowserGeneration;
    pluginBrowserWindow.reset();
    chooserBusy = false;
    if (!shuttingDown)
        updateFromEngine();
}

void MainComponent::choosePluginFile(int slotIndex)
{
    if (chooserBusy || shuttingDown
        || persistenceBlockedReason.isNotEmpty())
        return;
    chooserBusy = true;
    updateFromEngine();
    const auto generation = ++pluginBrowserGeneration;

    const auto chooserTitle = mic_daw::uiText(
        u8"VST3 효과 모듈을 선택하세요",
        "Select a VST3 effect module");
    constexpr auto chooserPattern = "*.vst3";

    fileChooser = std::make_unique<juce::FileChooser>(
        chooserTitle,
        getLastPluginDirectory(),
        chooserPattern);

    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::canSelectDirectories,
                             [safeThis = juce::Component::SafePointer<MainComponent>(this),
                              slotIndex, generation](const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr || safeThis->pluginBrowserGeneration != generation)
            return;

        const auto file = chooser.getResult();
        safeThis->chooserBusy = false;
        safeThis->updateFromEngine();

        if (file == juce::File{})
            return;

        safeThis->loadPluginIntoSlot(slotIndex, file);
    });
}

void MainComponent::loadPluginIntoSlot(int slotIndex, const juce::File& file)
{
    const auto currentCount = static_cast<int>(engine.getPluginSlots().size());
    if (slotIndex < 0 || slotIndex > currentCount || slotIndex >= AudioEngine::kMaxPluginSlots)
    {
        showError(mic_daw::uiText(u8"플러그인을 불러오지 못했습니다",
                                    "Could not load the plugin"),
                  mic_daw::uiText(
                      u8"선택하는 동안 효과 체인이 변경되었습니다. 다시 시도하세요.",
                      "The effects chain changed while you were selecting a plugin. Try again."));
        return;
    }

    // Remember the location even if loading fails; the change uses the same
    // quiet-period save as other preferences.
    rememberPluginDirectory(file.isDirectory() && !file.hasFileExtension("vst3")
                                ? file : file.getParentDirectory());
    closePluginEditor();
    const auto result = engine.loadPlugin(slotIndex, file);
    if (result.failed())
        showError(mic_daw::uiText(u8"플러그인을 불러오지 못했습니다",
                                    "Could not load the plugin"),
                  result.getErrorMessage());
    else
        settingsChanged();
    updateFromEngine();
}

void MainComponent::showPluginEditor(int slotIndex)
{
    if (chooserBusy || slotIndex < 0 || slotIndex >= AudioEngine::kMaxPluginSlots)
        return;

    auto* plugin = engine.getPlugin(slotIndex);
    if (plugin == nullptr)
        return;

    if (pluginEditorWindow != nullptr && editedPlugin == plugin
        && pluginEditorWindow->isVisible())
    {
        pluginEditorWindow->toFront(true);
        return;
    }

    closePluginEditor();
    plugin = engine.getPlugin(slotIndex);
    if (plugin == nullptr)
        return;

    const auto generation = ++pluginEditorGeneration;
    editedPlugin = plugin;
    editedPluginSlot = slotIndex;
    pluginEditorWindow = std::make_unique<PluginEditorWindow>(
        *plugin,
        [safeThis = juce::Component::SafePointer<MainComponent>(this), generation]
        {
            if (safeThis != nullptr
                && safeThis->pluginEditorGeneration == generation)
                safeThis->closePluginEditor();
        });
}

void MainComponent::closePluginEditor()
{
    const auto hadEditor = pluginEditorWindow != nullptr;
    ++pluginEditorGeneration;
    pluginEditorWindow.reset();
    editedPlugin = nullptr;
    editedPluginSlot = -1;
    if (hadEditor && !shuttingDown)
        settingsChanged();
}

void MainComponent::removePlugin(int slotIndex)
{
    if (chooserBusy || updatingControls)
        return;

    closePluginEditor();
    engine.clearPlugin(slotIndex);
    updateFromEngine();
    settingsChanged();
}

void MainComponent::movePlugin(int slotIndex, int destinationIndex)
{
    if (chooserBusy || updatingControls)
        return;

    closePluginEditor();
    if (engine.movePlugin(slotIndex, destinationIndex))
    {
        updateFromEngine();
        settingsChanged();
    }
}

void MainComponent::setPluginBypassed(int slotIndex, bool bypassed)
{
    if (chooserBusy || updatingControls)
        return;

    engine.setPluginBypassed(slotIndex, bypassed);
    updateFromEngine();
    settingsChanged();
}

void MainComponent::showError(const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                           title,
                                           message,
                                           mic_daw::uiText(u8"확인", "OK"),
                                           this);
}

juce::Colour MainComponent::colourForState(AudioEngine::EndpointState state)
{
    switch (state)
    {
        case AudioEngine::EndpointState::running:    return juce::Colour(0xff52d98c);
        case AudioEngine::EndpointState::waiting:    return juce::Colour(0xffffc857);
        case AudioEngine::EndpointState::recovering: return juce::Colour(0xff72a7ff);
        case AudioEngine::EndpointState::error:      return juce::Colour(0xffff6b6b);
        case AudioEngine::EndpointState::stopped:    return juce::Colour(0xff8c98aa);
    }

    return juce::Colours::white;
}

juce::String MainComponent::describeEndpoint(const AudioEngine::EndpointSnapshot& endpoint)
{
    auto text = endpoint.detail;
    if (endpoint.state == AudioEngine::EndpointState::running)
    {
        text = mic_daw::uiText(u8"정상 · ", "Running | ")
               + juce::String(endpoint.sampleRate / 1000.0, 1) + " kHz"
               + juce::String(u8" · ") + juce::String(endpoint.bufferSize) + " samples";
    }
    else if (endpoint.retryCount > 0)
    {
        text += mic_daw::uiText(u8" · 재시도 ", " | retry ")
                + juce::String(endpoint.retryCount) + mic_daw::uiText(u8"회", "");
    }

    return text;
}
