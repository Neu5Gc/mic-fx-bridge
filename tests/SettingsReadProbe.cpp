#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../src/SettingsDiskStore.h"

#include <exception>
#include <iostream>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <shellapi.h>
#endif

namespace
{
juce::String readArgument(int index, int argc, char* argv[])
{
   #if JUCE_WINDOWS
    // main's narrow argv can use a legacy Windows code page. Read UTF-16 so an
    // explicit settings path containing Korean is not corrupted by this probe.
    int wideCount = 0;
    if (auto** wideArguments = CommandLineToArgvW(GetCommandLineW(), &wideCount))
    {
        const auto value = juce::isPositiveAndBelow(index, wideCount)
            ? juce::String(wideArguments[index]) : juce::String();
        LocalFree(wideArguments);
        return value;
    }
   #endif
    return juce::isPositiveAndBelow(index, argc)
        ? juce::String::fromUTF8(argv[index]) : juce::String();
}

juce::File resolvePath(const juce::String& path)
{
    return juce::File::isAbsolutePath(path)
        ? juce::File(path) : juce::File::getCurrentWorkingDirectory().getChildFile(path);
}
} // namespace

int runSettingsReadProbe(int argc, char* argv[])
{
    if (argc < 2 || argc > 3)
    {
        std::cerr << "Usage: Vst3HostSmoke.exe --inspect-settings [settings-file]\n";
        return 2;
    }

    std::cout << std::unitbuf << std::boolalpha;
    try
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "MicVstBridge";
        options.filenameSuffix = "settings";
        options.folderName = "MicVstBridge";
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        options.doNotSave = true;
        options.millisecondsBeforeSaving = -1;

        const auto defaultFile = options.getDefaultFile();
        const auto file = argc == 3 ? resolvePath(readArgument(2, argc, argv)) : defaultFile;
        std::cout << "DEFAULT SETTINGS FILE: " << defaultFile.getFullPathName() << '\n';
        std::cout << "REQUESTED SETTINGS FILE: " << file.getFullPathName() << '\n';
        std::cout << "FILE: exists=" << file.existsAsFile() << "; bytes=" << file.getSize() << '\n';

        // No ApplicationProperties or AudioEngine is created. This opens just
        // the selected file, never initializes devices, and cannot save data.
        juce::PropertiesFile settings(file, options);
        const auto hasRackCount = settings.containsKey("pluginRackCount");
        const auto rackCount = settings.getIntValue("pluginRackCount", -1);
        std::cout << "PROPERTIES FILE: " << settings.getFile().getFullPathName() << '\n';
        std::cout << "PROPERTIES: valid=" << settings.isValidFile()
                  << "; count=" << settings.getAllProperties().size()
                  << "; hasPluginRackCount=" << hasRackCount
                  << "; pluginRackCount=" << rackCount << '\n';
        int guardedCount = 0;
        const auto startupValidation = mic_daw::validateStartupSettings(settings, 8, guardedCount);
        std::cout << "STARTUP GUARD: valid=" << startupValidation.wasOk()
                  << "; diskRackCount=" << guardedCount
                  << "; error=" << startupValidation.getErrorMessage() << '\n';

        for (int index = 0; index < juce::jlimit(0, 64, rackCount); ++index)
        {
            const auto prefix = "pluginSlot" + juce::String(index);
            const auto path = settings.getValue(prefix + "Path");
            const auto encodedState = settings.getValue(prefix + "State");
            juce::MemoryBlock state;
            const auto decoded = encodedState.isNotEmpty() && state.fromBase64Encoding(encodedState);
            const auto stateXml = decoded
                ? juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()))
                : nullptr;
            std::cout << "SLOT " << index + 1
                      << ": name=" << (path.isNotEmpty() ? resolvePath(path).getFileNameWithoutExtension() : juce::String())
                      << "; path=" << path
                      << "; format=" << settings.getValue(prefix + "Format")
                      << "; identifier=" << settings.getValue(prefix + "Identifier")
                      << "; bypassed=" << settings.getBoolValue(prefix + "Bypassed", false)
                      << "; stateChars=" << encodedState.length()
                      << "; stateDecoded=" << decoded
                      << "; stateBytes=" << state.getSize()
                      << "; stateXmlRoot=" << (stateXml != nullptr ? stateXml->getTagName() : juce::String("<none>"))
                      << '\n';
        }

        juce::XmlDocument document(file);
        const auto root = document.getDocumentElement();
        auto valueCount = 0;
        if (root != nullptr)
            for ([[maybe_unused]] auto* value : root->getChildWithTagNameIterator("VALUE"))
                ++valueCount;
        std::cout << "DIRECT XML: root=" << (root != nullptr ? root->getTagName() : juce::String("<none>"))
                  << "; valueCount=" << valueCount
                  << "; parseError=" << document.getLastParseError() << '\n';
        std::cout << "READ ONLY: no settings writes, plugins, UI, devices, or audio processing.\n";

        return file.existsAsFile() && settings.isValidFile() && root != nullptr
            && root->hasTagName("PROPERTIES") && startupValidation.wasOk() ? 0 : 3;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "ERROR: " << exception.what() << '\n';
        return 9;
    }
    catch (...)
    {
        std::cerr << "ERROR: Unknown C++ exception while inspecting settings\n";
        return 9;
    }
}
