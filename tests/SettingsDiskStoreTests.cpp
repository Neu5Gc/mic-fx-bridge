#include "SettingsDiskStore.h"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace
{
int checks = 0;

void require(bool condition, const char* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

juce::PropertiesFile::Options testOptions()
{
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = -1;
    return options;
}

struct TestSettings
{
    explicit TestSettings(const juce::File& file) : properties(file, testOptions()) {}
    ~TestSettings()
    {
        // Failed-save tests must not be retried implicitly by PropertiesFile's
        // destructor, which intentionally bypasses the guarded save helper.
        properties.setNeedsToBeSaved(false);
    }

    juce::PropertiesFile properties;
};

juce::File newCase(const juce::File& root, const juce::String& name)
{
    const auto directory = root.getChildFile(name);
    require(directory.isAChildOf(root), "Test directory escaped its owned fixture root");
    require(directory.createDirectory().wasOk(), "Unable to create test fixture directory");
    return directory.getChildFile("MicVstBridge.settings");
}

juce::File backupFor(const juce::File& file)
{
    return file.getSiblingFile(file.getFileName() + ".bak");
}

juce::File startupBackupFor(const juce::File& file)
{
    return file.getSiblingFile(file.getFileName() + ".startup-backup");
}

juce::MemoryBlock contentsOf(const juce::File& file)
{
    juce::MemoryBlock contents;
    require(file.loadFileAsData(contents), "Unable to read test fixture file");
    return contents;
}

void setRack(juce::PropertiesFile& settings, int count, const juce::String& generation)
{
    settings.setValue("pluginRackVersion", 1);
    settings.setValue("pluginRackCount", count);
    settings.setValue("generation", generation);
    settings.setValue("inputDevice", juce::String(u8"테스트 마이크"));
    for (int index = 0; index < count; ++index)
    {
        const auto prefix = "pluginSlot" + juce::String(index);
        settings.setValue(prefix + "Path", "test-only-plugin-" + juce::String(index) + ".vst3");
        settings.setValue(prefix + "State", "test-only-state-" + generation);
    }
}

void testPreviousSnapshotAndNoop(const juce::File& root)
{
    const auto file = newCase(root, "previous-and-noop");
    TestSettings holder(file);
    auto& settings = holder.properties;
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "An unchanged new file must be a no-op");
    require(!file.exists(), "An unchanged new file must not be created");

    setRack(settings, 2, "first");
    settings.setValue("pluginLibrary", "<LIBRARY><PLUGIN name=\"nested\"/></LIBRARY>");
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "The first settings save failed");
    const auto first = contentsOf(file);
    require(!backupFor(file).exists(), "First save must not invent a previous backup");

    setRack(settings, 2, "second");
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "The second settings save failed");
    require(contentsOf(backupFor(file)) == first, "Backup must preserve exact previous file bytes");
    require(contentsOf(file) != first, "Main settings must contain the current snapshot");
    require(!settings.needsToBeSaved(), "A successful save must clear the dirty flag");

    TestSettings reloaded(file);
    require(reloaded.properties.isValidFile(), "Written settings must reload as valid XML");
    require(reloaded.properties.getValue("generation") == "second", "Current generation did not persist");
    require(reloaded.properties.getValue("inputDevice") == juce::String(u8"테스트 마이크"),
            "Korean settings values must survive disk roundtrip");
    require(juce::parseXML(reloaded.properties.getValue("pluginLibrary")) != nullptr,
            "Nested XML settings values must survive disk roundtrip");

    const juce::Time sentinelTime(1700000000000LL);
    require(file.setLastModificationTime(sentinelTime), "Unable to set test file timestamp");
    require(backupFor(file).setLastModificationTime(sentinelTime), "Unable to set backup timestamp");
    const auto beforeMain = file.getLastModificationTime();
    const auto beforeBackup = backupFor(file).getLastModificationTime();
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "Unchanged settings save failed");
    require(file.getLastModificationTime() == beforeMain, "A no-op save rewrote the main file");
    require(backupFor(file).getLastModificationTime() == beforeBackup, "A no-op save rotated the backup");
    require(contentsOf(backupFor(file)) == first, "A no-op save changed backup contents");
}

void testMalformedPreviousPreservesBackup(const juce::File& root)
{
    const auto file = newCase(root, "malformed-previous");
    TestSettings holder(file);
    auto& settings = holder.properties;
    setRack(settings, 1, "known-good");
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "Fixture initial save failed");
    const auto good = contentsOf(file);
    setRack(settings, 1, "current");
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "Fixture backup save failed");

    int generation = 0;
    for (const auto* malformed : { "<PROPERTIES><VALUE name=\"truncated\"",
                                   "<OTHER><VALUE name=\"wrong-root\" val=\"1\"/></OTHER>",
                                   "<PROPERTIES/>",
                                   "<PROPERTIES><VALUE name=\"duplicate\" val=\"1\"/>"
                                   "<VALUE name=\"duplicate\" val=\"2\"/></PROPERTIES>" })
    {
        require(file.replaceWithText(malformed), "Unable to create malformed test fixture");
        settings.setValue("generation", ++generation);
        require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "Valid in-memory state could not replace malformed XML");
        require(contentsOf(backupFor(file)) == good, "Malformed main file overwrote a known-good backup");
        require(mic_daw::settings_disk_detail::parsePropertiesXml(contentsOf(file)) != nullptr,
                "Replacement main file is not meaningful properties XML");
    }
}

void testBackupFailureLeavesMainUntouched(const juce::File& root)
{
    const auto file = newCase(root, "backup-failure");
    TestSettings holder(file);
    auto& settings = holder.properties;
    setRack(settings, 1, "original");
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "Fixture initial save failed");
    const auto original = contentsOf(file);
    require(backupFor(file).createDirectory().wasOk(), "Unable to block the backup destination with a test directory");

    setRack(settings, 1, "pending");
    const auto result = mic_daw::saveSettingsWithBackup(settings);
    require(result.failed(), "Backup write failure must be reported");
    require(result.getErrorMessage().isNotEmpty(), "Backup failure must have an error message");
    require(contentsOf(file) == original, "A required backup failure must not overwrite the main file");
    require(settings.needsToBeSaved(), "Backup failure must retain dirty state for retry");
}

void testMainDiskFailureAndEmptySave(const juce::File& root)
{
    const auto caseFile = newCase(root, "main-disk-failure");
    const auto blocker = caseFile.getSiblingFile("not-a-directory");
    require(blocker.replaceWithText("owned test blocker"), "Unable to create path blocker fixture");
    TestSettings holder(blocker.getChildFile("MicVstBridge.settings"));
    setRack(holder.properties, 1, "pending");
    const auto failure = mic_daw::saveSettingsWithBackup(holder.properties);
    require(failure.failed(), "An impossible disk destination must report failure");
    require(holder.properties.needsToBeSaved(), "Disk failure must retain pending changes");
    require(blocker.loadFileAsString() == "owned test blocker", "A disk failure altered the blocking fixture");

    const auto emptyFile = newCase(root, "empty-save");
    TestSettings emptyHolder(emptyFile);
    setRack(emptyHolder.properties, 1, "must-survive");
    require(mic_daw::saveSettingsWithBackup(emptyHolder.properties).wasOk(), "Fixture initial save failed");
    const auto original = contentsOf(emptyFile);
    emptyHolder.properties.clear();
    require(mic_daw::saveSettingsWithBackup(emptyHolder.properties).failed(), "A meaningless empty settings write must be rejected");
    require(contentsOf(emptyFile) == original, "An empty write destroyed existing settings");
}

void testStartupSnapshotDoesNotRotate(const juce::File& root)
{
    const auto file = newCase(root, "startup-protection");
    TestSettings holder(file);
    auto& settings = holder.properties;
    setRack(settings, 2, "startup");
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "Fixture startup save failed");
    const auto startup = contentsOf(file);
    require(mic_daw::protectStartupSettings(settings).wasOk(), "Startup protection failed");
    require(contentsOf(startupBackupFor(file)) == startup, "Startup backup must preserve original bytes");

    setRack(settings, 2, "later-one");
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "Later save failed");
    const auto later = contentsOf(file);
    require(mic_daw::protectStartupSettings(settings).wasOk(), "Repeated startup protection failed");
    setRack(settings, 2, "later-two");
    require(mic_daw::saveSettingsWithBackup(settings).wasOk(), "Second later save failed");
    require(contentsOf(backupFor(file)) == later, "Rolling backup must track the previous saved snapshot");
    require(contentsOf(startupBackupFor(file)) == startup, "Autosaves or repeated protection rotated the startup snapshot");
}

void testStartupEmptyMalformedAndFailure(const juce::File& root)
{
    const auto emptyFile = newCase(root, "startup-empty");
    TestSettings emptyHolder(emptyFile);
    setRack(emptyHolder.properties, 0, "empty");
    require(mic_daw::saveSettingsWithBackup(emptyHolder.properties).wasOk(), "Empty rack fixture save failed");
    require(startupBackupFor(emptyFile).replaceWithText("prior startup backup sentinel"), "Unable to create prior-backup sentinel");
    require(mic_daw::protectStartupSettings(emptyHolder.properties).wasOk(), "An empty startup rack should be a no-op");
    require(startupBackupFor(emptyFile).loadFileAsString() == "prior startup backup sentinel",
            "Empty startup rack replaced a previous startup backup");

    const auto badFile = newCase(root, "startup-malformed");
    TestSettings badHolder(badFile);
    setRack(badHolder.properties, 1, "good-before-corruption");
    require(mic_daw::saveSettingsWithBackup(badHolder.properties).wasOk(), "Startup malformed fixture save failed");
    const auto good = contentsOf(badFile);
    require(badFile.copyFileTo(startupBackupFor(badFile)), "Unable to create known-good startup fixture");
    require(badFile.replaceWithText("<PROPERTIES>truncated"), "Unable to corrupt startup test fixture");
    require(mic_daw::protectStartupSettings(badHolder.properties).failed(), "Malformed startup XML must be reported");
    require(contentsOf(startupBackupFor(badFile)) == good, "Malformed startup XML replaced a valid startup backup");

    const auto blockedFile = newCase(root, "startup-write-failure");
    TestSettings blockedHolder(blockedFile);
    setRack(blockedHolder.properties, 1, "original");
    require(mic_daw::saveSettingsWithBackup(blockedHolder.properties).wasOk(), "Startup failure fixture save failed");
    const auto original = contentsOf(blockedFile);
    require(startupBackupFor(blockedFile).createDirectory().wasOk(), "Unable to block startup backup target");
    require(mic_daw::protectStartupSettings(blockedHolder.properties).failed(), "Startup backup failure must be reported");
    require(contentsOf(blockedFile) == original, "Startup backup failure modified the source file");
}

void testStartupDiskMemoryAgreement(const juce::File& root)
{
    const auto file = newCase(root, "startup-disk-memory-agreement");
    TestSettings writer(file);
    setRack(writer.properties, 6, "six-on-disk");
    writer.properties.setValue("nested", "<PLUGINS><PLUGIN name=\"test\"/></PLUGINS>");
    require(mic_daw::saveSettingsWithBackup(writer.properties).wasOk(), "Six-slot startup fixture save failed");
    const auto original = contentsOf(file);

    TestSettings startup(file);
    auto expected = -1;
    require(mic_daw::validateStartupSettings(startup.properties, 8, expected).wasOk(),
            "Unchanged startup settings with nested XML should validate");
    require(expected == 6, "Startup count must come from the independent disk read");
    require(!startup.properties.needsToBeSaved(), "Startup validation must not mark settings dirty");

    startup.properties.setValue("pluginRackCount", 0);
    require(mic_daw::validateStartupSettings(startup.properties, 8, expected).failed(),
            "Six slots on disk versus zero in memory must block startup persistence");
    require(expected == 6, "Memory count mismatch must retain the independently read count");
    require(contentsOf(file) == original, "Count mismatch validation changed the original disk file");

    startup.properties.removeValue("pluginRackCount");
    require(mic_daw::validateStartupSettings(startup.properties, 8, expected).failed(),
            "A missing in-memory rack count must not be accepted as an empty chain");
    require(expected == 6, "Missing memory count must retain the disk count");
    require(contentsOf(file) == original, "Missing-count validation changed the original disk file");

    startup.properties.setValue("pluginRackCount", 6);
    startup.properties.removeValue("pluginSlot2State");
    require(mic_daw::validateStartupSettings(startup.properties, 8, expected).failed(),
            "A missing in-memory plugin-state key must block even when the counts match");
    require(contentsOf(file) == original, "Missing-state validation changed the original file");
    require(!backupFor(file).exists(), "Read-only startup validation must not create a backup");
    require(!startupBackupFor(file).exists(), "Startup validation must not itself write a startup backup");
}

void testStartupFirstRunAndLegacy(const juce::File& root)
{
    const auto file = newCase(root, "startup-first-run");
    TestSettings fresh(file);
    auto expected = -1;
    require(mic_daw::validateStartupSettings(fresh.properties, 8, expected).wasOk(),
            "Missing file and empty memory must be a valid first run");
    require(expected == 0 && !file.exists(), "First-run validation must return zero without writing");
    fresh.properties.setValue("unexpected-pending-value", "keep");
    require(mic_daw::validateStartupSettings(fresh.properties, 8, expected).failed(),
            "Missing file with pending memory must not be called a fresh empty state");
    require(!file.exists(), "Rejected missing-file validation must not create the settings file");

    const auto legacyFile = newCase(root, "startup-legacy");
    TestSettings writer(legacyFile);
    writer.properties.setValue("pluginPath", "test-only-legacy.vst3");
    writer.properties.setValue("pluginState", "test-only-legacy-state");
    require(mic_daw::saveSettingsWithBackup(writer.properties).wasOk(), "Legacy fixture save failed");
    const auto legacy = contentsOf(legacyFile);
    TestSettings legacyStartup(legacyFile);
    require(mic_daw::validateStartupSettings(legacyStartup.properties, 8, expected).wasOk(),
            "A valid legacy pluginPath should validate");
    require(expected == 1, "Legacy pluginPath must produce one expected slot");
    require(contentsOf(legacyFile) == legacy, "Legacy validation must be read-only");
    require(mic_daw::validateStartupSettings(legacyStartup.properties, 0, expected).failed(),
            "Legacy state cannot validate when no slot is available");

    const auto zeroFile = newCase(root, "startup-explicit-zero");
    TestSettings zeroWriter(zeroFile);
    setRack(zeroWriter.properties, 0, "intentionally-empty");
    require(mic_daw::saveSettingsWithBackup(zeroWriter.properties).wasOk(), "Explicit zero fixture save failed");
    TestSettings zeroStartup(zeroFile);
    require(mic_daw::validateStartupSettings(zeroStartup.properties, 8, expected).wasOk(),
            "A valid explicitly empty rack should validate");
    require(expected == 0, "Explicit zero rack must remain zero");
}

void testFailedCaptureDoesNotWrite(const juce::File& root)
{
    const auto file = newCase(root, "capture-rollback");
    TestSettings writer(file);
    setRack(writer.properties, 2, "first");
    require(mic_daw::saveSettingsWithBackup(writer.properties).wasOk(), "Seed current settings");
    setRack(writer.properties, 2, "second");
    require(mic_daw::saveSettingsWithBackup(writer.properties).wasOk(), "Seed previous backup");
    const auto before = contentsOf(file);
    const auto backup = contentsOf(backupFor(file));
    const auto previous = writer.properties.getAllProperties();
    const auto failed = mic_daw::captureAndSaveSettings(writer.properties, [&]
    {
        writer.properties.setValue("pluginSlot0State", "fresh-first-plugin");
        writer.properties.setValue("inputDevice", "Changed device");
        return juce::Result::fail("Second plugin could not provide its current state");
    });
    require(failed.failed(), "Capture failure must remain visible");
    require(contentsOf(file) == before && contentsOf(backupFor(file)) == backup,
            "Failed mixed fresh/stale capture must not modify current file or backup");
    require(writer.properties.getAllProperties() == previous
                && !writer.properties.needsToBeSaved(),
            "Failed capture restores memory and its original dirty flag");
    require(mic_daw::captureAndSaveSettings(writer.properties, [&]
    {
        setRack(writer.properties, 2, "complete");
        return juce::Result::ok();
    }).wasOk(), "Successful retry writes a complete snapshot");
    require(contentsOf(backupFor(file)) == before, "Retry preserves the last complete file");
}

void testFinalCaptureIncludesPendingPreferences(const juce::File& root)
{
    const auto file = newCase(root, "final-capture-preferences");
    TestSettings writer(file);
    setRack(writer.properties, 1, "previous");
    writer.properties.setValue("lastPluginDirectory", "old-folder");
    writer.properties.setValue("vstSearchRoots", "old-search-root");
    writer.properties.setValue("bridgeEnabled", true);
    require(mic_daw::saveSettingsWithBackup(writer.properties).wasOk(), "Seed preferences");
    const auto previous = contentsOf(file);

    juce::StringPairArray preferences(false);
    preferences.set("lastPluginDirectory", "new-folder");
    preferences.set("vstSearchRoots", "old-search-root\nnew-search-root");
    preferences.set("bridgeEnabled", "1");
    const auto preflight = mic_daw::captureAndSaveSettings(writer.properties, [&]
    {
        writer.properties.setValue("pluginSlot0State", "partial");
        return juce::Result::fail("Live plugin callback lock is busy");
    }, preferences);
    require(preflight.failed() && contentsOf(file) == previous,
            "Failed live capture must preserve the disk snapshot");
    require(writer.properties.getValue("lastPluginDirectory") == "old-folder",
            "Failed capture must roll back memory before the final attempt");

    const auto finalCapture = mic_daw::captureAndSaveSettings(writer.properties, [&]
    {
        setRack(writer.properties, 1, "complete");
        writer.properties.setValue("bridgeEnabled", false); // stopped for final capture
        return juce::Result::ok();
    }, preferences);
    require(finalCapture.wasOk(), "Final capture succeeds after audio is stopped");
    TestSettings reloaded(file);
    require(reloaded.properties.getValue("lastPluginDirectory") == "new-folder"
                && reloaded.properties.getValue("vstSearchRoots") == "old-search-root\nnew-search-root",
            "Final capture must persist pending search preferences after failed preflight");
    require(reloaded.properties.getBoolValue("bridgeEnabled", false),
            "Final capture must persist intended restart state, not temporary stopped state");
    require(contentsOf(backupFor(file)) == previous,
            "Successful final capture backs up the last complete disk snapshot");
}

void testStartupInvalidXmlAndCounts(const juce::File& root)
{
    int index = 0;
    for (const auto* xml : { "", "<PROPERTIES>", "<PROPERTIES/>",
                             "<OTHER><VALUE name=\"pluginRackCount\" val=\"6\"/></OTHER>" })
    {
        const auto file = newCase(root, "startup-invalid-xml-" + juce::String(index++));
        require(file.replaceWithText(xml), "Unable to write invalid-startup XML fixture");
        const auto original = contentsOf(file);
        TestSettings startup(file);
        auto expected = -1;
        require(mic_daw::validateStartupSettings(startup.properties, 8, expected).failed(),
                "Invalid, zero-byte, or meaningless startup XML must be blocked");
        require(contentsOf(file) == original, "Invalid XML validation modified the source file");
    }

    index = 0;
    for (const auto* count : { "", "-1", "1.5", "six", "9", "2147483648",
                               "184467440737095516160000000000000000000" })
    {
        const auto file = newCase(root, "startup-invalid-count-" + juce::String(index++));
        TestSettings writer(file);
        writer.properties.setValue("pluginRackCount", count);
        require(mic_daw::saveSettingsWithBackup(writer.properties).wasOk(), "Count fixture save failed");
        const auto original = contentsOf(file);
        TestSettings startup(file);
        auto expected = -1;
        require(mic_daw::validateStartupSettings(startup.properties, 8, expected).failed(),
                "Invalid decimal or out-of-range count must be blocked without overflow");
        require(contentsOf(file) == original, "Invalid count validation modified the source file");
    }

    const auto leadingZeroFile = newCase(root, "startup-leading-zero-count");
    TestSettings writer(leadingZeroFile);
    writer.properties.setValue("pluginRackCount", " 0000000000000000000000000000000000000006 ");
    require(mic_daw::saveSettingsWithBackup(writer.properties).wasOk(), "Leading-zero fixture save failed");
    TestSettings startup(leadingZeroFile);
    auto expected = -1;
    require(mic_daw::validateStartupSettings(startup.properties, 8, expected).wasOk(),
            "Bounded decimal parsing must handle harmless leading zeroes and whitespace");
    require(expected == 6, "Leading-zero count was parsed incorrectly");
}
void testRetiredStartupMigration(const juce::File& root)
{
    for (int variant = 0; variant < 4; ++variant)
    {
        const auto file = newCase(root, "retired-startup-" + juce::String(variant));
        juce::XmlElement xml("PROPERTIES");
        const auto add = [&](const char* name, const char* value)
        {
            auto* entry = xml.createNewChildElement("VALUE");
            entry->setAttribute("name", name);
            entry->setAttribute("val", value);
        };
        add("pluginRackVersion", "1");
        add("pluginRackCount", "0");
        add("inputDevice", "Saved microphone");
        if (variant != 0) add("correctionEnabled", "invalid-retired-value");
        if (variant != 1) add("correctionProfileJson", "{malformed-retired-profile");
        require(xml.writeTo(file), "Seed exact legacy bytes");
        const auto original = contentsOf(file);
        TestSettings loaded(file);
        int expected = -1;
        require(mic_daw::validateStartupSettings(loaded.properties, 8, expected).wasOk()
            && expected == 0, "Retired fields do not block startup");
        require(mic_daw::protectStartupSettings(loaded.properties).wasOk()
            && contentsOf(startupBackupFor(file)) == original,
            "Even malformed retired-only state retains its original startup bytes");
        loaded.properties.setValue("generation", "new-version");
        require(mic_daw::saveSettingsWithBackup(loaded.properties).wasOk(), "Save active configuration");
        require(!loaded.properties.containsKey("correctionProfileJson")
            && !loaded.properties.containsKey("correctionEnabled"), "New active settings omit retired fields");
        require(contentsOf(backupFor(file)) == original
            && contentsOf(startupBackupFor(file)) == original,
            "Migration keeps old profile bytes in both protected backups");
        TestSettings restored(file);
        require(restored.properties.getValue("inputDevice") == "Saved microphone"
            && restored.properties.getValue("generation") == "new-version"
            && !restored.properties.containsKey("correctionProfileJson"),
            "Active device and other preferences survive restart");
        loaded.properties.setValue("generation", "later");
        require(mic_daw::saveSettingsWithBackup(loaded.properties).wasOk()
            && contentsOf(startupBackupFor(file)) == original,
            "Later saves cannot rotate away pre-removal startup bytes");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try
    {
        // Every run owns a new child directory. The optional argument is an
        // explicit workspace test-artifact parent, never a live settings path.
        const auto parent = argc > 1
                                ? juce::File(juce::String::fromUTF8(argv[1]))
                                : juce::File::getSpecialLocation(juce::File::tempDirectory);
        const auto root = parent.getChildFile("SettingsDiskStoreTests-" + juce::Uuid().toString());
        require(root.isAChildOf(parent) && !root.exists(), "Test fixture root is not a fresh child directory");
        require(root.createDirectory().wasOk(), "Unable to create owned test root");
        std::cout << "Test artifacts: " << root.getFullPathName().toStdString() << '\n';

        testPreviousSnapshotAndNoop(root);
        testRetiredStartupMigration(root);
        testMalformedPreviousPreservesBackup(root);
        testBackupFailureLeavesMainUntouched(root);
        testMainDiskFailureAndEmptySave(root);
        testStartupSnapshotDoesNotRotate(root);
        testStartupEmptyMalformedAndFailure(root);
        testStartupDiskMemoryAgreement(root);
        testStartupFirstRunAndLegacy(root);
        testStartupInvalidXmlAndCounts(root);
        testFailedCaptureDoesNotWrite(root);
        testFinalCaptureIncludesPendingPreferences(root);
        std::cout << "Settings disk store: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Settings disk store failed: " << error.what() << '\n';
        return 1;
    }
}
