#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include "UiText.h"
#include "LegacySettings.h"

#include <limits>

namespace mic_daw
{
namespace settings_disk_detail
{
inline bool isMeaningfulPropertiesXml(const juce::XmlElement* document)
{
    if (document == nullptr || !document->hasTagName("PROPERTIES"))
        return false;

    juce::StringArray names;
    for (auto* entry : document->getChildIterator())
    {
        if (!entry->hasTagName("VALUE"))
            return false;

        const auto name = entry->getStringAttribute("name");
        if (name.isEmpty() || names.contains(name)
            || (!entry->hasAttribute("val") && entry->getFirstChildElement() == nullptr))
            return false;

        names.add(name);
    }

    return !names.isEmpty();
}

inline std::unique_ptr<juce::XmlElement> parsePropertiesXml(const juce::MemoryBlock& data)
{
    if (data.getSize() == 0
        || data.getSize() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return nullptr;

    auto document = juce::parseXML(juce::String::createStringFromData(
        data.getData(), static_cast<int>(data.getSize())));
    return isMeaningfulPropertiesXml(document.get()) ? std::move(document) : nullptr;
}

inline bool hasNonemptyRack(const juce::XmlElement& document)
{
    for (auto* entry : document.getChildWithTagNameIterator("VALUE"))
        if (entry->getStringAttribute("name") == "pluginRackCount")
        {
            const auto count = entry->getStringAttribute("val").trim();
            return count.isNotEmpty() && count.containsOnly("0123456789")
                   && count.getIntValue() > 0;
        }

    return false;
}

inline juce::String storedPropertyValue(const juce::XmlElement& entry)
{
    if (const auto* child = entry.getFirstChildElement())
        return child->toString(
            juce::XmlElement::TextFormat().singleLine().withoutHeader());

    return entry.getStringAttribute("val");
}

inline const juce::XmlElement* findProperty(const juce::XmlElement& document,
                                             const juce::String& name)
{
    for (auto* entry : document.getChildWithTagNameIterator("VALUE"))
        if (entry->getStringAttribute("name") == name)
            return entry;

    return nullptr;
}

inline bool hasMaterialUserState(const juce::XmlElement& document)
{
    if (hasNonemptyRack(document))
        return true;
    // Old fields are opaque retired data. Preserve exact bytes before migration,
    // including incomplete/malformed profiles, without validating removed DSP.
    for (auto* entry : document.getChildWithTagNameIterator("VALUE"))
        if (isRetiredSetting(entry->getStringAttribute("name")))
            return true;
    return false;
}

inline juce::Result writeBackupAtomically(const juce::File& destination,
                                          const juce::MemoryBlock& contents)
{
    if (destination.isDirectory())
        return juce::Result::fail(
            uiText(u8"백업 경로에 같은 이름의 폴더가 있습니다: ",
                   "A folder with the same name exists at the backup path: ")
            + destination.getFullPathName());

    const auto directoryResult = destination.getParentDirectory().createDirectory();
    if (directoryResult.failed())
        return juce::Result::fail(
            uiText(u8"백업 폴더를 만들지 못했습니다: ",
                   "Could not create the backup folder: ")
            + directoryResult.getErrorMessage());

    juce::TemporaryFile temporary(destination);
    {
        juce::FileOutputStream output(temporary.getFile());
        if (!output.openedOk() || !output.write(contents.getData(), contents.getSize()))
            return juce::Result::fail(
                uiText(u8"설정 백업을 기록하지 못했습니다: ",
                       "Could not write the settings backup: ")
                + destination.getFullPathName());

        output.flush();
        if (output.getStatus().failed())
            return juce::Result::fail(
                uiText(u8"설정 백업을 디스크에 반영하지 못했습니다: ",
                       "Could not flush the settings backup to disk: ")
                + output.getStatus().getErrorMessage());
    }

    juce::MemoryBlock verified;
    if (!temporary.getFile().loadFileAsData(verified) || verified != contents
        || parsePropertiesXml(verified) == nullptr)
        return juce::Result::fail(
            uiText(u8"기록한 설정 백업을 검증하지 못했습니다: ",
                   "Could not verify the written settings backup: ")
            + destination.getFullPathName());

    if (!temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail(
            uiText(u8"설정 백업 파일을 교체하지 못했습니다: ",
                   "Could not replace the settings backup file: ")
            + destination.getFullPathName());

    return juce::Result::ok();
}

// PropertiesFile embeds XML-valued properties as child elements, while
// PropertySet::createXml always string-escapes them. Mirror the actual disk
// serialization here so nested XML can also be verified after saving.
inline std::unique_ptr<juce::XmlElement> makePropertiesXml(juce::PropertiesFile& settings)
{
    auto document = std::make_unique<juce::XmlElement>("PROPERTIES");
    const auto& properties = settings.getAllProperties();
    for (int index = 0; index < properties.size(); ++index)
    {
        auto* entry = document->createNewChildElement("VALUE");
        entry->setAttribute("name", properties.getAllKeys()[index]);
        const auto& value = properties.getAllValues()[index];
        if (auto child = juce::parseXML(value))
            entry->addChildElement(child.release());
        else
            entry->setAttribute("val", value);
    }

    return document;
}
} // namespace settings_disk_detail

// Independently inspect the disk before startup restore or any write. In
// particular, a valid six-slot file must not be accepted as an empty rack just
// because PropertiesFile's in-memory representation was empty or altered.
[[nodiscard]] inline juce::Result validateStartupSettings(juce::PropertiesFile& settings,
                                                         int maximumSlots,
                                                         int& expectedSlotCount)
{
    expectedSlotCount = 0;
    const juce::ScopedLock settingsLock(settings.getLock());
    const auto& file = settings.getFile();
    if (maximumSlots < 0 || file == juce::File())
        return juce::Result::fail(
            uiText(u8"시작 설정을 확인할 경로나 슬롯 한도가 올바르지 않습니다.",
                   "The startup settings path or slot limit is invalid."));

    if (!file.exists())
    {
        if (settings.isValidFile() && settings.getAllProperties().size() == 0
            && !settings.needsToBeSaved())
            return juce::Result::ok();

        return juce::Result::fail(
            uiText(u8"설정 파일은 없지만 메모리에 설정이 남아 있어 새 빈 체인으로 처리하지 않습니다.",
                   "The settings file is missing, but settings remain in memory, so they will not be treated as a new empty chain."));
    }

    juce::MemoryBlock bytes;
    if (!file.existsAsFile() || !file.loadFileAsData(bytes))
        return juce::Result::fail(
            uiText(u8"시작 설정 파일을 읽지 못해 덮어쓰기를 차단했습니다: ",
                   "The startup settings file could not be read, so overwriting was blocked: ")
            + file.getFullPathName());

    const auto document = settings_disk_detail::parsePropertiesXml(bytes);
    if (document == nullptr)
        return juce::Result::fail(
            uiText(u8"시작 설정 파일이 비어 있거나 올바른 속성 XML이 아니어서 덮어쓰기를 차단했습니다.",
                   "The startup settings file is empty or is not valid properties XML, so overwriting was blocked."));

    // Match PropertiesFile::loadAsXml normalization for XML-valued properties,
    // then compare all actual entries (not values inherited from fallbacks).
    juce::StringPairArray diskProperties(false);
    for (auto* entry : document->getChildWithTagNameIterator("VALUE"))
    {
        const auto value = settings_disk_detail::storedPropertyValue(*entry);
        diskProperties.set(entry->getStringAttribute("name"), value);
    }


    if (diskProperties.containsKey("pluginRackCount"))
    {
        const auto count = diskProperties.getValue("pluginRackCount", {}).trim();
        if (count.isEmpty() || !count.containsOnly("0123456789"))
            return juce::Result::fail(
                uiText(u8"저장된 효과 체인 개수가 올바른 0 이상의 정수가 아닙니다.",
                       "The saved effect-chain count is not a valid non-negative integer."));

        int parsedCount = 0;
        for (auto* digit = count.toRawUTF8(); *digit != '\0'; ++digit)
        {
            const auto value = static_cast<int>(*digit - '0');
            if (parsedCount > maximumSlots / 10
                || (parsedCount == maximumSlots / 10 && value > maximumSlots % 10))
                return juce::Result::fail(
                    uiText(u8"저장된 효과 체인 개수가 지원 슬롯 수를 초과해 덮어쓰기를 차단했습니다.",
                           "The saved effect-chain count exceeds the supported number of slots, so overwriting was blocked."));
            parsedCount = parsedCount * 10 + value;
        }
        expectedSlotCount = parsedCount;
    }
    else if (diskProperties.getValue("pluginPath", {}).isNotEmpty())
    {
        expectedSlotCount = 1;
        if (maximumSlots < 1)
            return juce::Result::fail(
                uiText(u8"기존 플러그인을 복원할 슬롯이 없습니다.",
                       "No slot is available to restore the existing plugin."));
    }

    if (!settings.isValidFile() || settings.getAllProperties() != diskProperties)
        return juce::Result::fail(
            uiText(u8"디스크의 시작 설정과 읽어 온 설정이 다릅니다. 저장된 효과 ",
                   "The startup settings on disk do not match the loaded settings. The saved chain of ")
            + juce::String(expectedSlotCount)
            + uiText(u8"개를 빈 체인이나 다른 설정으로 덮어쓰지 않습니다.",
                     " effect(s) will not be overwritten by an empty chain or different settings."));

    return juce::Result::ok();
}

// Use with PropertiesFile::storeAsXML and millisecondsBeforeSaving = -1. This
// function owns the explicit save sequence; it does not alter plugin state.
[[nodiscard]] inline juce::Result saveSettingsWithBackup(juce::PropertiesFile& settings)
{
    const juce::ScopedLock settingsLock(settings.getLock());
    stripRetiredSettings(settings);
    if (!settings.needsToBeSaved())
        return juce::Result::ok();

    const auto pending = settings_disk_detail::makePropertiesXml(settings);
    if (!settings_disk_detail::isMeaningfulPropertiesXml(pending.get()))
        return juce::Result::fail(
            uiText(u8"저장할 설정이 비어 있거나 올바른 속성 XML이 아니어서 기존 파일을 유지했습니다.",
                   "The settings to save are empty or are not valid properties XML, so the existing file was kept."));

    juce::StringPairArray pendingProperties(false);
    for (auto* entry : pending->getChildWithTagNameIterator("VALUE"))
        pendingProperties.set(entry->getStringAttribute("name"),
                              settings_disk_detail::storedPropertyValue(*entry));


    const auto& file = settings.getFile();
    if (file == juce::File() || file.isDirectory())
        return juce::Result::fail(
            uiText(u8"설정 파일 경로에 기록할 수 없습니다.",
                   "The settings file path is not writable."));

    if (file.existsAsFile())
    {
        juce::MemoryBlock previous;
        if (!file.loadFileAsData(previous))
            return juce::Result::fail(
                uiText(u8"기존 설정 파일을 읽지 못해 덮어쓰지 않았습니다: ",
                       "The existing settings file could not be read, so it was not overwritten: ")
                + file.getFullPathName());

        // A malformed current file must never replace a known-good backup.
        // It can still be replaced by a validated, current in-memory snapshot.
        if (settings_disk_detail::parsePropertiesXml(previous) != nullptr)
        {
            const auto backup = file.getSiblingFile(file.getFileName() + ".bak");
            const auto result = settings_disk_detail::writeBackupAtomically(backup, previous);
            if (result.failed())
                return result;
        }
    }

    // JUCE's PropertiesFile::saveAsXml -> XmlElement::writeTo writes through a
    // sibling TemporaryFile, flushes it, and only then replaces the target.
    if (!settings.saveIfNeeded())
        return juce::Result::fail(
            uiText(u8"설정 파일을 디스크에 저장하지 못했습니다: ",
                   "Could not save the settings file to disk: ")
            + file.getFullPathName());

    juce::MemoryBlock saved;
    auto savedDocument = file.loadFileAsData(saved)
                             ? settings_disk_detail::parsePropertiesXml(saved)
                             : nullptr;
    if (savedDocument == nullptr || !savedDocument->isEquivalentTo(pending.get(), true))
    {
        settings.setNeedsToBeSaved(true);
        return juce::Result::fail(
            uiText(u8"저장된 설정 파일을 검증하지 못했습니다. 이전 백업을 유지합니다: ",
                   "Could not verify the saved settings file. The previous backup will be kept: ")
            + file.getFullPathName());
    }

    return juce::Result::ok();
}

// Do not let one failed plugin capture publish a partially refreshed snapshot.
// This is shared by autosave and final exit, including preserved unavailable slots.
template <typename Capture>
[[nodiscard]] inline juce::Result captureAndSaveSettings(juce::PropertiesFile& settings,
                                                        Capture&& capture,
                                                        const juce::StringPairArray& preferences = {})
{
    const juce::PropertySet previous(settings);
    const auto wasDirty = settings.needsToBeSaved();
    const auto captured = capture();
    if (captured.failed())
    {
        settings.clear();
        settings.addAllPropertiesFrom(previous);
        settings.setNeedsToBeSaved(wasDirty);
        return captured;
    }
    for (int index = 0; index < preferences.size(); ++index)
        settings.setValue(preferences.getAllKeys()[index], preferences.getAllValues()[index]);
    return saveSettingsWithBackup(settings);
}

// Call before startup restore/edits. It protects material user state (a
// nonempty rack or opaque retired settings) only once per settings path
// per process; later autosaves cannot rotate away this snapshot.
[[nodiscard]] inline juce::Result protectStartupSettings(juce::PropertiesFile& settings)
{
    static juce::CriticalSection protectionLock;
    static juce::StringArray handledPaths;
    const juce::ScopedLock startupLock(protectionLock);
    const juce::ScopedLock settingsLock(settings.getLock());

    const auto& file = settings.getFile();
    const auto path = file.getFullPathName();
    if (handledPaths.contains(path, true))
        return juce::Result::ok();

    if (!file.exists())
    {
        handledPaths.add(path);
        return juce::Result::ok();
    }

    juce::MemoryBlock original;
    if (!file.existsAsFile() || !file.loadFileAsData(original))
        return juce::Result::fail(
            uiText(u8"시작 시점의 효과 체인 설정을 읽지 못했습니다: ",
                   "Could not read the effect-chain settings captured at startup: ")
            + path);

    const auto document = settings_disk_detail::parsePropertiesXml(original);
    if (document == nullptr)
        return juce::Result::fail(
            uiText(u8"시작 설정 XML이 올바르지 않아 기존 시작 백업을 유지했습니다: ",
                   "The startup settings XML is invalid, so the existing startup backup was kept: ")
            + path);

    juce::StringPairArray diskProperties(false);
    for (auto* entry : document->getChildWithTagNameIterator("VALUE"))
        diskProperties.set(entry->getStringAttribute("name"),
                           settings_disk_detail::storedPropertyValue(*entry));


    if (settings_disk_detail::hasMaterialUserState(*document))
    {
        const auto backup = file.getSiblingFile(file.getFileName() + ".startup-backup");
        const auto result = settings_disk_detail::writeBackupAtomically(backup, original);
        if (result.failed())
            return result;
    }

    handledPaths.add(path);
    return juce::Result::ok();
}
} // namespace mic_daw
