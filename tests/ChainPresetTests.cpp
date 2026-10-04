#include "ChainPreset.h"

#include <array>
#include <iostream>
#include <stdexcept>

namespace
{
int checks = 0;

void require(bool condition, const char* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

struct Fixture
{
    juce::File directory = juce::File::getCurrentWorkingDirectory().getChildFile(
        "chain-tests-" + juce::Uuid().toString());
    std::vector<juce::File> files;

    Fixture() { require(directory.createDirectory().wasOk(), "Create fixture directory"); }
    ~Fixture()
    {
        for (const auto& file : files)
            static_cast<void>(file.deleteFile());
        static_cast<void>(directory.deleteFile());
    }
    juce::File file(const juce::String& name)
    {
        auto result = directory.getChildFile(name);
        files.push_back(result);
        return result;
    }
};

mic_daw::ChainPreset chain(int count = 3)
{
    mic_daw::ChainPreset preset;
    constexpr std::array<unsigned char, 8> bytes { 0, 255, 1, 128, 2, 0, 192, 42 };
    for (int index = 0; index < count; ++index)
    {
        mic_daw::ChainPreset::Slot slot;
        slot.path = "C:\\Program Files\\VST3\\Effect " + juce::String(index) + ".vst3";
        slot.identifier = "VST3-Effect-" + juce::String(index);
        slot.stateBase64 = juce::MemoryBlock(bytes.data(), bytes.size()).toBase64Encoding();
        slot.bypassed = index % 2 == 1;
        preset.slots.push_back(std::move(slot));
    }
    return preset;
}

void requireEqual(const mic_daw::ChainPreset& actual, const mic_daw::ChainPreset& expected)
{
    require(actual.slots.size() == expected.slots.size(), "Exact slot count");
    for (std::size_t index = 0; index < expected.slots.size(); ++index)
    {
        const auto& a = actual.slots[index];
        const auto& e = expected.slots[index];
        require(a.path == e.path && a.identifier == e.identifier
                    && a.formatName == e.formatName && a.stateBase64 == e.stateBase64
                    && a.bypassed == e.bypassed,
                "Exact ordered metadata, bypass and binary state");
    }
}

void testRoundtripAndAtomicWrite(Fixture& fixture)
{
    auto original = chain(8);
    original.slots[0].path = mic_daw::uiText(u8"C:\\효과\\테스트 & EQ.vst3",
                                           "C:\\Effects\\Test & EQ.vst3");
    const auto file = fixture.file("roundtrip.micfxchain");
    require(mic_daw::ChainPresetCodec::write(file, original).wasOk(), "Write versioned chain");
    mic_daw::ChainPreset restored;
    require(mic_daw::ChainPresetCodec::read(file, restored).wasOk(), "Read versioned chain");
    requireEqual(restored, original);
    const auto savedBytes = file.loadFileAsString();
    auto invalid = original;
    invalid.slots[1].stateBase64 = "99999999.A";
    require(mic_daw::ChainPresetCodec::write(file, invalid).failed(), "Reject oversized state prefix");
    require(file.loadFileAsString() == savedBytes, "Failed export preserves destination bytes");
    const auto legacyDestination = fixture.file("protected.settings");
    require(legacyDestination.replaceWithText("existing settings"), "Seed legacy destination");
    require(mic_daw::ChainPresetCodec::write(legacyDestination, original).failed(),
            "Explicit export cannot overwrite shared settings format");
    require(legacyDestination.loadFileAsString() == "existing settings", "Protect legacy file");
    mic_daw::ChainPreset empty;
    require(mic_daw::ChainPresetCodec::write(file, empty).wasOk(), "Empty chain is explicit and valid");
    require(mic_daw::ChainPresetCodec::read(file, restored).wasOk(), "Read empty chain");
    require(restored.slots.empty(), "Empty chain roundtrip");
}

void testMalformedPreservesDestination(Fixture& fixture)
{
    const auto file = fixture.file("invalid.micfxchain");
    const auto original = chain();
    const auto reject = [&](const juce::String& xml)
    {
        require(file.replaceWithText(xml), "Write malformed fixture");
        auto restored = original;
        require(mic_daw::ChainPresetCodec::read(file, restored).failed(), "Reject malformed file");
        requireEqual(restored, original);
    };
    reject("<MIC_FX_CHAIN version=\"2\" count=\"0\"/>");
    reject("<MIC_FX_CHAIN version=\"1\" count=\"9\"/>");
    reject("<MIC_FX_CHAIN version=\"1\" count=\"-1\"/>");
    reject("<MIC_FX_CHAIN version=\"1\" count=\"1\"/>");
    reject("<MIC_FX_CHAIN version=\"1\" count=\"0\"><SLOT/></MIC_FX_CHAIN>");
    reject("<wrong/>");
    for (const auto* state : { "", "8.A", "-1.A", "2147483647.A", "08.A", "8.!!!!!!!!!!!", "0." })
    {
        auto invalid = original;
        invalid.slots[0].stateBase64 = state;
        require(mic_daw::ChainPresetCodec::validate(invalid).failed(), "Reject unsafe state encoding");
    }
    auto invalid = original;
    invalid.slots[0].formatName = "VST";
    require(mic_daw::ChainPresetCodec::validate(invalid).failed(), "VST3 only");
    invalid = original;
    invalid.slots[0].path = "relative.vst3";
    require(mic_daw::ChainPresetCodec::validate(invalid).failed(), "Require exact absolute module path");
    invalid = original;
    invalid.slots[0].identifier.clear();
    require(mic_daw::ChainPresetCodec::validate(invalid).failed(), "Require exact plugin class");
}

void testLegacyImport(Fixture& fixture)
{
    const auto original = chain(6);
    juce::XmlElement xml("PROPERTIES");
    const auto put = [&](const juce::String& name, const juce::String& value)
    {
        auto* item = xml.createNewChildElement("VALUE");
        item->setAttribute("name", name);
        item->setAttribute("val", value);
    };
    put("pluginRackVersion", "1");
    put("pluginRackCount", "6");
    put("inputDevice", "Must not import devices");
    put("correctionProfileJson", "Unrelated invalid correction does not block chain recovery");
    auto* device = xml.createNewChildElement("VALUE");
    device->setAttribute("name", "deviceXml");
    device->createNewChildElement("DEVICESETUP");
    for (std::size_t i = 0; i < original.slots.size(); ++i)
    {
        const auto prefix = "pluginSlot" + juce::String(static_cast<int>(i));
        const auto& slot = original.slots[i];
        put(prefix + "Path", slot.path);
        put(prefix + "Identifier", slot.identifier);
        put(prefix + "Format", slot.formatName);
        put(prefix + "State", slot.stateBase64);
        put(prefix + "Bypassed", slot.bypassed ? "1" : "0");
    }
    const auto file = fixture.file("legacy.settings.startup-backup");
    require(file.replaceWithText(xml.toString()), "Write synthetic six-slot backup");
    const auto before = file.loadFileAsString();
    mic_daw::ChainPreset restored;
    require(mic_daw::ChainPresetCodec::read(file, restored).wasOk(), "Read six-slot legacy backup");
    requireEqual(restored, original);
    require(file.loadFileAsString() == before, "Legacy read never writes original settings");
    put("pluginSlot6Path", "C:\\Unexpected.vst3");
    require(file.replaceWithText(xml.toString()), "Write trailing slot");
    require(mic_daw::ChainPresetCodec::read(file, restored).failed(), "Reject concealed trailing slot");
    requireEqual(restored, original);
    put("pluginRackCount", "0");
    require(file.replaceWithText(xml.toString()), "Write duplicate property");
    require(mic_daw::ChainPresetCodec::read(file, restored).failed(), "Reject duplicate legacy fields");
}

struct FakeSlot
{
    static inline int alive = 0;
    explicit FakeSlot(juce::String id) : identifier(std::move(id)) { ++alive; }
    ~FakeSlot() { --alive; }
    juce::String identifier;
};

void testTransactionalStaging()
{
    auto preset = chain();
    std::vector<std::unique_ptr<FakeSlot>> live;
    live.push_back(std::make_unique<FakeSlot>("original"));
    const auto* original = live[0].get();
    for (int failedIndex = 0; failedIndex < 3; ++failedIndex)
    {
        int calls = 0;
        bool committed = false;
        const auto result = mic_daw::stageChainPreset<FakeSlot>(preset,
            [&](const auto& source, auto& slot)
            {
                slot = std::make_unique<FakeSlot>(source.identifier);
                return calls++ == failedIndex ? juce::Result::fail("Injected install/state/prepare failure")
                                              : juce::Result::ok();
            },
            [&](auto& staged)
            {
                committed = true;
                live.swap(staged);
                return juce::Result::ok();
            });
        require(result.failed() && !committed, "No publication after any slot staging failure");
        require(live.size() == 1 && live[0].get() == original, "Preserve original live instance");
        require(FakeSlot::alive == 1, "Release all staged instances on failure");
    }
    const auto rejectedCommit = mic_daw::stageChainPreset<FakeSlot>(preset,
        [](const auto& source, auto& slot)
        {
            slot = std::make_unique<FakeSlot>(source.identifier);
            return juce::Result::ok();
        },
        [](auto&) { return juce::Result::fail("Output format changed"); });
    require(rejectedCommit.failed() && live[0].get() == original && FakeSlot::alive == 1,
            "Pre-publication output change preserves live rack and releases replacements");
    int commits = 0;
    const auto successful = mic_daw::stageChainPreset<FakeSlot>(preset,
        [](const auto& source, auto& slot)
        {
            slot = std::make_unique<FakeSlot>(source.identifier);
            return juce::Result::ok();
        },
        [&](auto& staged)
        {
            ++commits;
            require(staged.size() == 3, "Entire replacement ready before publication");
            live.swap(staged);
            return juce::Result::ok();
        });
    require(successful.wasOk() && commits == 1 && FakeSlot::alive == 3, "Single complete publication");
    for (std::size_t i = 0; i < live.size(); ++i)
        require(live[i]->identifier == preset.slots[i].identifier, "Published exact order");
}
} // namespace

int main()
{
    try
    {
        Fixture fixture;
        testRoundtripAndAtomicWrite(fixture);
        testMalformedPreservesDestination(fixture);
        testLegacyImport(fixture);
        testTransactionalStaging();
        require(FakeSlot::alive == 0, "All fake runtimes released");
        std::cout << "ChainPresetTests: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "ChainPresetTests failed: " << error.what() << '\n';
        return 1;
    }
}
