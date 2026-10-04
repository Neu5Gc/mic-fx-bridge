#pragma once

#include <juce_core/juce_core.h>

#include "UiText.h"

#include <vector>
#include <memory>

namespace mic_daw
{
struct ChainPreset
{
    struct Slot
    {
        juce::String path;
        juce::String identifier;
        juce::String formatName = "VST3";
        juce::String stateBase64;
        bool bypassed = false;
    };

    std::vector<Slot> slots;
};

class ChainPresetCodec final
{
public:
    static constexpr int maxSlots = 8;
    static constexpr int maxStateBytes = 16 * 1024 * 1024;
    static constexpr int maxTotalStateBytes = 64 * 1024 * 1024;
    static constexpr juce::int64 maxFileBytes = 96 * 1024 * 1024;

    // JUCE's MemoryBlock encoding contains a decimal allocation size and its
    // decoder accepts truncated data. Check size and canonical encoding before
    // passing any imported state to a plugin (or allocating the declared size).
    [[nodiscard]] static juce::Result validate(const ChainPreset& preset)
    {
        if (preset.slots.size() > maxSlots)
            return invalid();

        int totalBytes = 0;
        for (std::size_t index = 0; index < preset.slots.size(); ++index)
        {
            const auto& slot = preset.slots[index];
            const auto badSlot = [&]
            {
                return juce::Result::fail(uiText(u8"FX 슬롯 ", "FX slot ")
                    + juce::String(static_cast<int>(index + 1))
                    + uiText(u8"의 경로, 식별자 또는 설정 데이터가 올바르지 않습니다.",
                             " has an invalid path, identifier, or settings data."));
            };
            if (slot.path.isEmpty() || slot.path.length() > 32767
                || !juce::File::isAbsolutePath(slot.path)
                || !slot.path.endsWithIgnoreCase(".vst3")
                || slot.identifier.isEmpty() || slot.identifier.length() > 4096
                || slot.formatName != "VST3")
                return badSlot();

            const auto dot = slot.stateBase64.indexOfChar('.');
            int bytes = 0;
            if (dot <= 0 || dot > 8
                || !parseInteger(slot.stateBase64.substring(0, dot), maxStateBytes, bytes)
                || bytes == 0)
                return badSlot();
            const auto encoded = slot.stateBase64.substring(dot + 1);
            if (encoded.length() != (bytes * 8 + 5) / 6
                || !encoded.containsOnly(".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"))
                return badSlot();
            totalBytes += bytes;
            if (totalBytes > maxTotalStateBytes)
                return invalid();
            juce::MemoryBlock decoded;
            if (!decoded.fromBase64Encoding(slot.stateBase64)
                || decoded.toBase64Encoding() != slot.stateBase64)
                return badSlot();
        }
        return juce::Result::ok();
    }

    [[nodiscard]] static juce::Result read(const juce::File& file, ChainPreset& preset)
    {
        juce::FileInputStream stream(file);
        if (!stream.openedOk() || stream.getTotalLength() <= 0
            || stream.getTotalLength() > maxFileBytes)
            return juce::Result::fail(uiText(u8"체인 파일을 읽을 수 없거나 크기 제한을 초과했습니다.",
                                            "The chain file cannot be read or exceeds the size limit."));
        juce::MemoryBlock contents;
        const auto length = static_cast<std::size_t>(stream.getTotalLength());
        if (stream.readIntoMemoryBlock(contents, length) != length || stream.getStatus().failed())
            return invalid();
        auto xml = juce::parseXML(juce::String::createStringFromData(
            contents.getData(), static_cast<int>(contents.getSize())));
        if (xml == nullptr)
            return invalid();

        ChainPreset candidate;
        const auto parsed = xml->hasTagName("MIC_FX_CHAIN")
                                ? readChain(*xml, candidate) : readLegacy(*xml, candidate);
        if (parsed.failed())
            return parsed;
        const auto checked = validate(candidate);
        if (checked.failed())
            return checked;
        preset = std::move(candidate);
        return juce::Result::ok();
    }

    [[nodiscard]] static juce::Result write(const juce::File& file, const ChainPreset& preset)
    {
        const auto checked = validate(preset);
        if (checked.failed())
            return checked;
        // Explicit exports never overwrite the shared legacy settings/backups.
        if (!file.hasFileExtension("micfxchain") || file.isDirectory())
            return juce::Result::fail(uiText(u8".micfxchain 확장자의 파일을 선택하세요.",
                                            "Choose a file with the .micfxchain extension."));

        juce::XmlElement xml("MIC_FX_CHAIN");
        xml.setAttribute("version", 1);
        xml.setAttribute("count", static_cast<int>(preset.slots.size()));
        for (const auto& slot : preset.slots)
        {
            auto* entry = xml.createNewChildElement("SLOT");
            entry->setAttribute("path", slot.path);
            entry->setAttribute("identifier", slot.identifier);
            entry->setAttribute("format", slot.formatName);
            entry->setAttribute("bypassed", slot.bypassed ? "1" : "0");
            entry->setAttribute("state", slot.stateBase64);
        }
        const auto text = xml.toString();
        juce::TemporaryFile temporary(file);
        {
            juce::FileOutputStream output(temporary.getFile());
            if (!output.openedOk() || !output.write(text.toRawUTF8(), text.getNumBytesAsUTF8()))
                return writeFailed();
            output.flush();
            if (output.getStatus().failed())
                return writeFailed();
        }
        ChainPreset verified;
        if (read(temporary.getFile(), verified).failed() || !equal(preset, verified)
            || !temporary.overwriteTargetFileWithTemporary())
            return writeFailed();
        return juce::Result::ok();
    }

private:
    static juce::Result invalid()
    {
        return juce::Result::fail(uiText(u8"체인 파일의 형식, 버전 또는 슬롯 수가 올바르지 않습니다.",
                                       "The chain file format, version, or slot count is invalid."));
    }

    static juce::Result writeFailed()
    {
        return juce::Result::fail(uiText(u8"체인 파일을 안전하게 저장하지 못했습니다. 기존 파일은 유지됩니다.",
                                       "The chain file could not be saved safely. The existing file is retained."));
    }

    static bool parseInteger(const juce::String& text, int maximum, int& value)
    {
        if (text.isEmpty() || text.length() > 8 || !text.containsOnly("0123456789"))
            return false;
        const auto parsed = text.getLargeIntValue();
        if (parsed < 0 || parsed > maximum || juce::String(parsed) != text)
            return false;
        value = static_cast<int>(parsed);
        return true;
    }

    static bool parseBypass(const juce::String& text, bool& value)
    {
        if (text != "0" && text != "1" && text != "false" && text != "true")
            return false;
        value = text == "1" || text == "true";
        return true;
    }

    static juce::Result readChain(const juce::XmlElement& xml, ChainPreset& candidate)
    {
        int count = 0;
        if (xml.getStringAttribute("version") != "1"
            || !parseInteger(xml.getStringAttribute("count"), maxSlots, count)
            || xml.getNumChildElements() != count)
            return invalid();
        for (const auto* entry : xml.getChildIterator())
        {
            if (!entry->hasTagName("SLOT") || entry->getFirstChildElement() != nullptr)
                return invalid();
            ChainPreset::Slot slot;
            slot.path = entry->getStringAttribute("path");
            slot.identifier = entry->getStringAttribute("identifier");
            slot.formatName = entry->getStringAttribute("format");
            slot.stateBase64 = entry->getStringAttribute("state");
            if (!parseBypass(entry->getStringAttribute("bypassed"), slot.bypassed))
                return invalid();
            candidate.slots.push_back(std::move(slot));
        }
        return juce::Result::ok();
    }

    static juce::Result readLegacy(const juce::XmlElement& xml, ChainPreset& candidate)
    {
        if (!xml.hasTagName("PROPERTIES"))
            return invalid();
        juce::StringPairArray values;
        for (const auto* entry : xml.getChildIterator())
        {
            const auto name = entry->getStringAttribute("name");
            if (!entry->hasTagName("VALUE") || name.isEmpty() || values.containsKey(name))
                return invalid();
            // Device XML/correction data may be nested; neither is imported.
            values.set(name, entry->getStringAttribute("val"));
        }
        int count = 0;
        const auto rack = values.containsKey("pluginRackCount");
        if (rack)
        {
            if (values.getValue("pluginRackVersion", {}) != "1"
                || !parseInteger(values.getValue("pluginRackCount", {}), maxSlots, count))
                return invalid();
        }
        else if (values.getValue("pluginPath", {}).isNotEmpty())
            count = 1;
        else
            return invalid();

        // Reject hidden/trailing slot fields instead of silently dropping them.
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
            slot.path = values.getValue(prefix + "Path", {});
            slot.identifier = values.getValue(prefix + "Identifier", {});
            slot.formatName = rack ? values.getValue(prefix + "Format", {}) : juce::String("VST3");
            slot.stateBase64 = values.getValue(prefix + "State", {});
            if (!parseBypass(values.getValue(prefix + "Bypassed", {}), slot.bypassed))
                return invalid();
            candidate.slots.push_back(std::move(slot));
        }
        return juce::Result::ok();
    }

    static bool equal(const ChainPreset& left, const ChainPreset& right)
    {
        if (left.slots.size() != right.slots.size())
            return false;
        for (std::size_t i = 0; i < left.slots.size(); ++i)
        {
            const auto& a = left.slots[i];
            const auto& b = right.slots[i];
            if (a.path != b.path || a.identifier != b.identifier || a.formatName != b.formatName
                || a.stateBase64 != b.stateBase64 || a.bypassed != b.bypassed)
                return false;
        }
        return true;
    }
};

// The commit callback is reached only after every replacement has been built.
// Factories own plugin installation/state/prepare checks; callers own the short
// callback-safe publication step. Kept injectable for failure regression tests.
template <typename RuntimeSlot, typename Factory, typename Commit>
[[nodiscard]] juce::Result stageChainPreset(const ChainPreset& preset,
                                           Factory&& factory, Commit&& commit)
{
    const auto checked = ChainPresetCodec::validate(preset);
    if (checked.failed())
        return checked;
    std::vector<std::unique_ptr<RuntimeSlot>> staged;
    staged.reserve(ChainPresetCodec::maxSlots);
    for (std::size_t index = 0; index < preset.slots.size(); ++index)
    {
        std::unique_ptr<RuntimeSlot> slot;
        const auto result = factory(preset.slots[index], slot);
        if (result.failed())
            return juce::Result::fail(uiText(u8"FX 슬롯 ", "FX slot ")
                + juce::String(static_cast<int>(index + 1)) + ": " + result.getErrorMessage());
        if (slot == nullptr)
            return juce::Result::fail(uiText(u8"FX 슬롯을 만들지 못했습니다.",
                                            "The FX slot could not be created."));
        staged.push_back(std::move(slot));
    }
    return commit(staged);
}
} // namespace mic_daw
