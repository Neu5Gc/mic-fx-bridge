#pragma once

#include "ChainPreset.h"
#include "LegacySettings.h"

namespace mic_daw
{
// A full portable copy of the canonical PropertiesFile, including preferences
// owned by the UI. This is deliberately distinct from a chain-only preset.
struct SettingsSnapshot
{
    juce::StringPairArray properties { false };
};

struct DecodedSettingsSnapshot
{
    juce::String inputDevice;
    juce::String outputDevice;
    int inputChannelMode = 0;
    bool bridgeEnabled = true;
    bool autoRecover = true;
    ChainPreset chain;
};

class SettingsSnapshotCodec final
{
public:
    static constexpr juce::int64 maxFileBytes = ChainPresetCodec::maxFileBytes;

    [[nodiscard]] static juce::Result decode(const SettingsSnapshot& snapshot,
                                            DecodedSettingsSnapshot& destination)
    {
        const auto& values = snapshot.properties;
        if (values.size() > 256)
            return invalid();
        juce::int64 totalBytes = 0;
        for (int i = 0; i < values.size(); ++i)
        {
            const auto& key = values.getAllKeys()[i];
            if (key.isEmpty() || key.length() > 256)
                return invalid();
            totalBytes += static_cast<juce::int64>(values.getAllValues()[i].getNumBytesAsUTF8());
            if (totalBytes > maxFileBytes)
                return invalid();
        }

        DecodedSettingsSnapshot candidate;
        if (!values.containsKey("inputDevice") || !values.containsKey("outputDevice")
            || !parseBool(values["bridgeEnabled"], candidate.bridgeEnabled)
            || !parseBool(values["autoRecover"], candidate.autoRecover)
            || !parseInt(values["inputChannelMode"], 2, candidate.inputChannelMode))
            return invalid();
        candidate.inputDevice = values["inputDevice"];
        candidate.outputDevice = values["outputDevice"];
        if (candidate.inputDevice.length() > 32767 || candidate.outputDevice.length() > 32767)
            return invalid();

        int count = 0;
        const auto rack = values.containsKey("pluginRackCount");
        if (rack)
        {
            if (values["pluginRackVersion"] != "1"
                || !parseInt(values["pluginRackCount"], ChainPresetCodec::maxSlots, count))
                return invalid();
        }
        else if (values["pluginPath"].isNotEmpty())
            count = 1;
        else if (values.containsKey("pluginRackVersion"))
            return invalid();
        // Pre-rack settings with no plugin are also valid full settings.
        for (const auto& name : values.getAllKeys())
        {
            if (!name.startsWith("pluginSlot"))
                continue;
            bool known = false;
            for (int index = 0; index < count && rack; ++index)
                for (const auto* suffix : { "Path", "Identifier", "Format", "State", "Bypassed" })
                    known = known || name == "pluginSlot" + juce::String(index) + suffix;
            if (!known)
                return invalid();
        }
        for (int index = 0; index < count; ++index)
        {
            const auto prefix = rack ? "pluginSlot" + juce::String(index) : juce::String("plugin");
            ChainPreset::Slot slot;
            slot.path = values[prefix + "Path"];
            slot.identifier = values[prefix + "Identifier"];
            slot.formatName = rack ? values[prefix + "Format"] : juce::String("VST3");
            slot.stateBase64 = values[prefix + "State"];
            if (!parseBool(values[prefix + "Bypassed"], slot.bypassed))
                return invalid();
            candidate.chain.slots.push_back(std::move(slot));
        }
        const auto checked = ChainPresetCodec::validate(candidate.chain);
        if (checked.failed())
            return checked;
        destination = std::move(candidate);
        return juce::Result::ok();
    }

    [[nodiscard]] static juce::Result validate(const SettingsSnapshot& snapshot)
    {
        DecodedSettingsSnapshot ignored;
        return decode(snapshot, ignored);
    }

    [[nodiscard]] static juce::Result fromProperties(const juce::PropertySet& source,
                                                    SettingsSnapshot& destination)
    {
        const juce::ScopedLock lock(source.getLock());
        juce::PropertySet copy(source);
        SettingsSnapshot candidate { copy.getAllProperties() };
        stripRetiredSettings(candidate.properties);
        const auto result = validate(candidate);
        if (result.wasOk())
            destination = std::move(candidate);
        return result;
    }

    [[nodiscard]] static juce::Result applyToProperties(const SettingsSnapshot& snapshot,
                                                       juce::PropertySet& destination)
    {
        const auto result = validate(snapshot);
        if (result.failed())
            return result;
        const juce::ScopedLock lock(destination.getLock());
        destination.clear();
        for (int i = 0; i < snapshot.properties.size(); ++i)
            if (!isRetiredSetting(snapshot.properties.getAllKeys()[i]))
                destination.setValue(snapshot.properties.getAllKeys()[i],
                                     snapshot.properties.getAllValues()[i]);
        return juce::Result::ok();
    }

    [[nodiscard]] static juce::Result read(const juce::File& file, SettingsSnapshot& destination)
    {
        // Never interpret a chain export as complete settings, even if renamed
        // file contents happen to look like a legacy PropertiesFile.
        if (file.hasFileExtension("micfxchain"))
            return invalid();
        juce::FileInputStream stream(file);
        const auto length = stream.getTotalLength();
        if (!stream.openedOk() || length <= 0 || length > maxFileBytes)
            return juce::Result::fail(mic_daw::uiText("The settings file cannot be read or exceeds the size limit."));
        juce::MemoryBlock contents;
        if (stream.readIntoMemoryBlock(contents, static_cast<std::size_t>(length))
                != static_cast<std::size_t>(length) || stream.getStatus().failed())
            return invalid();
        const auto xml = juce::parseXML(juce::String::createStringFromData(
            contents.getData(), static_cast<int>(contents.getSize())));
        if (xml == nullptr || !xml->hasTagName("PROPERTIES") || xml->getNumChildElements() > 256)
            return invalid();
        SettingsSnapshot candidate;
        for (const auto* entry : xml->getChildIterator())
        {
            const auto name = entry->getStringAttribute("name");
            if (!entry->hasTagName("VALUE") || name.isEmpty()
                || candidate.properties.containsKey(name) || entry->getNumChildElements() > 1
                || (!entry->hasAttribute("val") && entry->getFirstChildElement() == nullptr)
                || (entry->getFirstChildElement() != nullptr && entry->hasAttribute("val")))
                return invalid();
            const auto value = entry->getFirstChildElement() != nullptr
                ? entry->getFirstChildElement()->toString(
                    juce::XmlElement::TextFormat().singleLine().withoutHeader())
                : entry->getStringAttribute("val");
            candidate.properties.set(name, value);
        }
        const auto checked = validate(candidate);
        if (checked.wasOk())
        {
            stripRetiredSettings(candidate.properties);
            destination = std::move(candidate);
        }
        return checked;
    }

    [[nodiscard]] static juce::Result write(const juce::File& file, const SettingsSnapshot& snapshot)
    {
        const auto checked = validate(snapshot);
        if (checked.failed())
            return checked;
        if (!file.hasFileExtension("settings") || file.isDirectory())
            return juce::Result::fail(mic_daw::uiText("Choose a file with the .settings extension."));
        if (file.getParentDirectory().createDirectory().failed())
            return writeFailed();
        SettingsSnapshot cleaned = snapshot;
        stripRetiredSettings(cleaned.properties);
        juce::XmlElement xml("PROPERTIES");
        for (int i = 0; i < cleaned.properties.size(); ++i)
        {
            auto* entry = xml.createNewChildElement("VALUE");
            entry->setAttribute("name", cleaned.properties.getAllKeys()[i]);
            entry->setAttribute("val", cleaned.properties.getAllValues()[i]);
        }
        const auto text = xml.toString();
        if (text.getNumBytesAsUTF8() > static_cast<std::size_t>(maxFileBytes))
            return writeFailed();
        juce::TemporaryFile temporary(file);
        {
            juce::FileOutputStream output(temporary.getFile());
            if (!output.openedOk() || !output.write(text.toRawUTF8(), text.getNumBytesAsUTF8()))
                return writeFailed();
            output.flush();
            if (output.getStatus().failed())
                return writeFailed();
        }
        SettingsSnapshot verified;
        if (read(temporary.getFile(), verified).failed()
            || verified.properties != cleaned.properties
            || !temporary.overwriteTargetFileWithTemporary())
            return writeFailed();
        return juce::Result::ok();
    }

private:
    static juce::Result invalid()
    {
        return juce::Result::fail(mic_daw::uiText("This is not a valid full settings file. The current settings are retained."));
    }
    static juce::Result writeFailed()
    {
        return juce::Result::fail(mic_daw::uiText("The settings file could not be saved safely. The existing file is retained."));
    }
    static bool parseBool(const juce::String& text, bool& value)
    {
        if (text != "0" && text != "1" && text != "false" && text != "true")
            return false;
        value = text == "1" || text == "true";
        return true;
    }
    static bool parseInt(const juce::String& text, int maximum, int& value)
    {
        if (text.isEmpty() || text.length() > 2 || !text.containsOnly("0123456789"))
            return false;
        value = text.getIntValue();
        return value <= maximum && juce::String(value) == text;
    }
};

// Preparation and durable write are both fallible. The publication callback is
// only reached after they succeed; it must not report a rollback after publishing.
// This same orchestration is used by AudioEngine and injectable failure tests.
template <typename RuntimeSlot, typename SlotFactory, typename BeforeCommit, typename Publish>
[[nodiscard]] juce::Result stageSettingsSnapshot(
    const SettingsSnapshot& snapshot, SlotFactory&& slotFactory,
    BeforeCommit&& beforeCommit, Publish&& publish)
{
    DecodedSettingsSnapshot decoded;
    std::vector<std::unique_ptr<RuntimeSlot>> slots;
    try
    {
        const auto checked = SettingsSnapshotCodec::decode(snapshot, decoded);
        if (checked.failed())
            return checked;
        const auto staged = stageChainPreset<RuntimeSlot>(decoded.chain,
            std::forward<SlotFactory>(slotFactory),
            [&](auto& candidates)
            {
                slots.swap(candidates);
                return juce::Result::ok();
            });
        if (staged.failed())
            return staged;
        const auto durable = beforeCommit();
        if (durable.failed())
            return durable;
    }
    catch (...)
    {
        return juce::Result::fail(mic_daw::uiText("Settings preparation failed; the current settings are retained."));
    }
    publish(decoded, slots);
    return juce::Result::ok();
}
} // namespace mic_daw
