#include "SettingsSnapshot.h"

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
        "settings-tests-" + juce::Uuid().toString());
    std::vector<juce::File> files;
    Fixture() { require(directory.createDirectory().wasOk(), "Create isolated fixture"); }
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

mic_daw::SettingsSnapshot settings()
{
    mic_daw::SettingsSnapshot snapshot;
    auto& p = snapshot.properties;
    p.set("bridgeEnabled", "1");
    p.set("autoRecover", "0");
    p.set("inputDevice", juce::String(u8"마이크 & Input"));
    p.set("outputDevice", "Virtual output");
    p.set("inputChannelMode", "1");
    p.set("lastPluginDirectory", "C:\\Synthetic\\Effects");
    p.set("vstSearchRoots", "C:\\Synthetic\\Effects\nD:\\More");
    p.set("futurePreference", "Preserve this value <exactly>");
    p.set("pluginRackVersion", "1");
    p.set("pluginRackCount", "2");
    for (int i = 0; i < 2; ++i)
    {
        const auto prefix = "pluginSlot" + juce::String(i);
        p.set(prefix + "Path", "C:\\Synthetic\\Effect" + juce::String(i) + ".vst3");
        p.set(prefix + "Identifier", "synthetic-effect-" + juce::String(i));
        p.set(prefix + "Format", "VST3");
        p.set(prefix + "Bypassed", i == 1 ? "1" : "0");
        const unsigned char state[] { 0, 127, 255, static_cast<unsigned char>(i) };
        p.set(prefix + "State", juce::MemoryBlock(state, sizeof(state)).toBase64Encoding());
    }
    return snapshot;
}

void testRoundtrip(Fixture& fixture)
{
    const auto source = settings();
    const auto file = fixture.file("roundtrip.settings");
    require(mic_daw::SettingsSnapshotCodec::write(file, source).wasOk(), "Write full settings");
    mic_daw::SettingsSnapshot restored;
    require(mic_daw::SettingsSnapshotCodec::read(file, restored).wasOk(), "Read full settings");
    require(restored.properties == source.properties, "Exact full property and binary-state roundtrip");
    mic_daw::DecodedSettingsSnapshot decoded;
    require(mic_daw::SettingsSnapshotCodec::decode(restored, decoded).wasOk(), "Decode full settings");
    require(decoded.inputDevice == source.properties["inputDevice"]
        && decoded.outputDevice == "Virtual output" && decoded.inputChannelMode == 1
        && decoded.bridgeEnabled && !decoded.autoRecover, "Device, channel and run preferences");
    require(decoded.chain.slots.size() == 2 && decoded.chain.slots[1].bypassed,
            "Rack order and bypass retained");
    juce::PropertySet properties;
    properties.setValue("obsoletePreference", "remove me");
    require(mic_daw::SettingsSnapshotCodec::applyToProperties(restored, properties).wasOk(),
            "Apply full properties");
    require(!properties.containsKey("obsoletePreference"), "Full import replaces old preferences");
    mic_daw::SettingsSnapshot copied;
    require(mic_daw::SettingsSnapshotCodec::fromProperties(properties, copied).wasOk()
        && copied.properties == source.properties, "PropertySet compatibility and search preferences");
    const auto firstRun = fixture.file("first-run/initial.settings");
    require(mic_daw::SettingsSnapshotCodec::write(firstRun, source).wasOk()
        && mic_daw::SettingsSnapshotCodec::read(firstRun, copied).wasOk(),
        "First-run save creates the missing settings directory");
    static_cast<void>(firstRun.deleteFile());
    static_cast<void>(firstRun.getParentDirectory().deleteFile());
    for (const auto* suffix : { ".bak", ".startup-backup" })
    {
        const auto backup = fixture.file(juce::String("legacy.settings") + suffix);
        require(file.copyFileTo(backup), "Create backup fixture");
        require(mic_daw::SettingsSnapshotCodec::read(backup, copied).wasOk()
            && copied.properties == source.properties, "Read legacy settings backup");
    }
    auto xml = juce::parseXML(file);
    auto* nested = xml->createNewChildElement("VALUE");
    nested->setAttribute("name", "legacyXmlPreference");
    nested->createNewChildElement("DEVICE")->setAttribute("name", "Synthetic");
    require(xml->writeTo(file), "Write legacy nested XML value");
    require(mic_daw::SettingsSnapshotCodec::read(file, restored).wasOk()
        && restored.properties["legacyXmlPreference"].contains("Synthetic"),
        "Nested PropertiesFile values remain compatible");
}

void testRejectedData(Fixture& fixture)
{
    const auto valid = settings();
    const auto target = fixture.file("protected.settings");
    require(mic_daw::SettingsSnapshotCodec::write(target, valid).wasOk(), "Seed protected file");
    const auto original = target.loadFileAsString();
    const auto reject = [&](const mic_daw::SettingsSnapshot& candidate)
    {
        require(mic_daw::SettingsSnapshotCodec::validate(candidate).failed(), "Reject invalid snapshot");
        require(mic_daw::SettingsSnapshotCodec::write(target, candidate).failed()
            && target.loadFileAsString() == original, "Invalid save preserves original file");
        juce::PropertySet live;
        live.setValue("sentinel", "untouched");
        require(mic_daw::SettingsSnapshotCodec::applyToProperties(candidate, live).failed()
            && live.getValue("sentinel") == "untouched" && live.getAllProperties().size() == 1,
            "Invalid import preserves in-memory properties");
    };
    auto candidate = valid;
    candidate.properties.remove("inputDevice");
    reject(candidate);
    candidate.properties.set("InputDevice", "Wrong case");
    reject(candidate);
    for (const auto* key : { "bridgeEnabled", "autoRecover", "inputChannelMode" })
    {
        candidate = valid;
        candidate.properties.set(key, "garbage");
        reject(candidate);
    }
    candidate = valid;
    candidate.properties.set("inputChannelMode", "3");
    reject(candidate);
    candidate = valid;
    candidate.properties.set("pluginRackCount", "9");
    reject(candidate);
    candidate = valid;
    candidate.properties.set("pluginSlot7Path", "C:\\Hidden.vst3");
    reject(candidate);
    candidate = valid;
    candidate.properties.set("pluginSlot0State", "16777217.A");
    reject(candidate);
    candidate = valid;
    candidate.properties.set("pluginSlot1State", "4.A");
    reject(candidate);
    const auto malformed = fixture.file("malformed.settings");
    for (const auto* text : { "<PROPERTIES>", "<MIC_FX_CHAIN version=\"1\" count=\"0\"/>",
             "<PROPERTIES><VALUE name=\"inputDevice\" val=\"a\"/><VALUE name=\"inputDevice\" val=\"b\"/></PROPERTIES>" })
    {
        require(malformed.replaceWithText(text), "Write malformed fixture");
        candidate = valid;
        require(mic_daw::SettingsSnapshotCodec::read(malformed, candidate).failed()
            && candidate.properties == valid.properties, "Read failure leaves destination unchanged");
    }
    const auto chainExtension = fixture.file("wrong.micfxchain");
    require(target.copyFileTo(chainExtension), "Seed mislabeled full file");
    require(mic_daw::SettingsSnapshotCodec::read(chainExtension, candidate).failed(),
            "Reject chain files as full settings");
    require(mic_daw::SettingsSnapshotCodec::write(chainExtension, valid).failed(),
            "Full snapshots require settings extension");
    require(mic_daw::SettingsSnapshotCodec::write(
        target.getChildFile("new.settings"), valid).failed(),
        "Filesystem failure is reported");
}

void testFullTransaction()
{
    const auto source = settings();
    mic_daw::SettingsSnapshot live = source;
    live.properties.set("inputDevice", "Current input");
    const auto original = live;
    int prepareCalls = 0;
    int writeCalls = 0;
    int publications = 0;
    int failSlot = -1;
    bool failWrite = false;
    bool throwPreparation = false;
    const auto apply = [&](const mic_daw::SettingsSnapshot& candidate)
    {
        prepareCalls = 0;
        return mic_daw::stageSettingsSnapshot<int>(candidate,
            [&](const mic_daw::ChainPreset::Slot&, std::unique_ptr<int>& slot)
            {
                if (throwPreparation)
                    throw std::runtime_error("Synthetic plugin preparation failure");
                if (prepareCalls++ == failSlot)
                    return juce::Result::fail("Synthetic missing plugin or preparation failure");
                slot = std::make_unique<int>(prepareCalls);
                return juce::Result::ok();
            },
            [&]
            {
                ++writeCalls;
                return failWrite ? juce::Result::fail("Synthetic disk failure") : juce::Result::ok();
            },
            [&](auto& decoded, auto& slots)
            {
                ++publications;
                require(slots.size() == 2, "Publish receives all prepared objects");
                require(decoded.inputDevice == source.properties["inputDevice"]
                    && decoded.inputChannelMode == 1 && decoded.bridgeEnabled && !decoded.autoRecover,
                    "Publish receives complete configuration");
                live = candidate;
            });
    };
    throwPreparation = true;
    require(apply(source).failed() && live.properties == original.properties
        && writeCalls == 0 && publications == 0, "Preparation exception preserves live settings");
    throwPreparation = false;
    for (int slot = 0; slot < 2; ++slot)
    {
        failSlot = slot;
        require(apply(source).failed() && live.properties == original.properties
            && writeCalls == 0 && publications == 0,
            "Missing or invalid plugin at any position preserves full configuration");
    }
    failSlot = -1;
    failWrite = true;
    require(apply(source).failed() && live.properties == original.properties
        && prepareCalls == 2 && writeCalls == 1 && publications == 0,
        "Disk failure after successful preparation still preserves all live configuration");
    failWrite = false;
    auto invalid = source;
    invalid.properties.set("pluginSlot0State", "broken");
    require(apply(invalid).failed() && live.properties == original.properties
        && prepareCalls == 0 && writeCalls == 1 && publications == 0,
        "Malformed state stops before factories and publication");
    require(apply(source).wasOk() && live.properties == source.properties
        && writeCalls == 2 && publications == 1, "Complete snapshot publishes exactly once after durable write");
}
void testRetiredSettings(Fixture& fixture)
{
    const auto current = settings();
    for (int variant = 0; variant < 4; ++variant)
    {
        auto legacy = current;
        if (variant != 1)
            legacy.properties.set("correctionProfileJson", variant == 2 ? "{broken" : "{\"version\":2}");
        if (variant != 0)
            legacy.properties.set("correctionEnabled", variant == 3 ? "not-a-bool" : "1");
        mic_daw::DecodedSettingsSnapshot decoded;
        require(mic_daw::SettingsSnapshotCodec::decode(legacy, decoded).wasOk()
            && decoded.chain.slots.size() == 2 && decoded.inputChannelMode == 1,
            "Retired malformed/partial fields cannot block live configuration");
        juce::PropertySet destination;
        require(mic_daw::SettingsSnapshotCodec::applyToProperties(legacy, destination).wasOk()
            && !destination.containsKey("correctionProfileJson")
            && !destination.containsKey("correctionEnabled"), "Import drops retired fields");
        require(destination.getValue("futurePreference") == current.properties["futurePreference"],
            "Unrelated future preferences are preserved");
        const auto file = fixture.file("retired-" + juce::String(variant) + ".settings");
        require(mic_daw::SettingsSnapshotCodec::write(file, legacy).wasOk(), "Write migrated settings");
        mic_daw::SettingsSnapshot restored;
        require(mic_daw::SettingsSnapshotCodec::read(file, restored).wasOk()
            && restored.properties == current.properties, "New files contain only active settings");
        require(legacy.properties.containsKey("correctionProfileJson")
            || legacy.properties.containsKey("correctionEnabled"), "Migration does not mutate its source");
        // Read an actual legacy PropertiesFile, not only a new sanitized export.
        juce::XmlElement xml("PROPERTIES");
        for (int i = 0; i < legacy.properties.size(); ++i)
        {
            auto* entry = xml.createNewChildElement("VALUE");
            entry->setAttribute("name", legacy.properties.getAllKeys()[i]);
            entry->setAttribute("val", legacy.properties.getAllValues()[i]);
        }
        require(xml.writeTo(file), "Write old-version fixture");
        const auto originalBytes = file.loadFileAsString();
        require(mic_daw::SettingsSnapshotCodec::read(file, restored).wasOk()
            && restored.properties == current.properties && file.loadFileAsString() == originalBytes,
            "Reading an old profile ignores it without editing the file");
    }
}

} // namespace

int main()
{
    try
    {
        Fixture fixture;
        testRoundtrip(fixture);
        testRejectedData(fixture);
        testFullTransaction();
        testRetiredSettings(fixture);
        std::cout << "SettingsSnapshotTests: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SettingsSnapshotTests failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
