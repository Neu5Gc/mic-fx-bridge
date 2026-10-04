// Exercise the production scanner without a window, audio devices, plugins, or
// application settings. The test define excludes all GUI code from this TU.
#ifndef MIC_VST_BRIDGE_DISCOVERY_TEST
#define MIC_VST_BRIDGE_DISCOVERY_TEST 1
#endif
#include "../src/PluginBrowser.cpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace
{
int failures = 0;
int checks = 0;

void expect(bool condition, std::string_view description)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::cerr << "FAILED: " << description << '\n';
    }
}

struct Fixture
{
    const juce::File temporaryRoot = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const juce::File root = temporaryRoot.getChildFile(
        "mic-vst-discovery-fixture-" + juce::Uuid().toString());

    Fixture()
    {
        if (root.exists() || root.createDirectory().failed())
            throw std::runtime_error("Unable to create an isolated discovery fixture");
    }

    ~Fixture()
    {
        // Delete only this test's unique, validated temporary directory.
        if (root.getParentDirectory() == temporaryRoot
            && root.getFileName().startsWith("mic-vst-discovery-fixture-"))
        {
            if (!root.deleteRecursively())
                std::cerr << "Fixture cleanup failed: " << root.getFullPathName() << '\n';
        }
    }

    juce::File directory(const juce::String& relativePath) const
    {
        const auto result = root.getChildFile(relativePath);
        if (!result.isAChildOf(root) || result.createDirectory().failed())
            throw std::runtime_error("Unable to create fixture directory");
        return result;
    }

    juce::File file(const juce::String& relativePath) const
    {
        const auto result = root.getChildFile(relativePath);
        if (!result.isAChildOf(root)
            || result.getParentDirectory().createDirectory().failed()
            || result.create().failed())
            throw std::runtime_error("Unable to create fixture file");
        return result;
    }
};

std::shared_ptr<ScanState> scanRoots(const juce::StringArray& roots,
                                    const ScanLimits& limits = {})
{
    auto result = std::make_shared<ScanState>();
    discoverFiles(result, roots, limits);
    expect(result->finished.load(std::memory_order_acquire), "scan reports completion");
    expect(result->error.isEmpty(), "fixture scan has no error");
    return result;
}

bool contains(const ScanState& result, const juce::File& file)
{
    return std::find(result.pendingFiles.begin(), result.pendingFiles.end(), file)
           != result.pendingFiles.end();
}

void testDiscoveryAndDeduplication(const Fixture& fixture)
{
    const auto standardRoot = fixture.directory("plugins");
    const auto single = fixture.file("plugins/Simple.vst3");
    const auto upper = fixture.file("plugins/Vendor/Upper.VST3");
    const auto korean = fixture.file(juce::String(u8"plugins/한글 경로/보컬 효과.vst3"));
    const auto bundle = fixture.directory("plugins/Vendor/Bundle.vst3");
    const auto innerBinary = fixture.file("plugins/Vendor/Bundle.vst3/Contents/x86_64-win/Bundle.vst3");
    const auto ordinaryDll = fixture.file("plugins/NotValidatedAsVst.dll");
    fixture.file("plugins/Readme.txt");

    const auto result = scanRoots({ standardRoot.getFullPathName(),
                                    standardRoot.getFullPathName().toUpperCase(),
                                    standardRoot.getChildFile("Vendor").getFullPathName(),
                                    innerBinary.getParentDirectory().getFullPathName(),
                                    fixture.root.getChildFile("missing").getFullPathName(),
                                    "relative/folder", "\\\\invalid-host\\unvisited-share" });
    expect(contains(*result, single), "standalone VST3 file is found");
    expect(contains(*result, upper), "VST3 extension matching ignores case");
    expect(contains(*result, korean), "Unicode filename/path is preserved");
    expect(contains(*result, bundle), "VST3 bundle is a single candidate");
    expect(!contains(*result, innerBinary), "bundle's inner module is not duplicated");
    expect(!result->limitReached, "small directory tree completes within budgets");

    expect(!contains(*result, ordinaryDll), "VST3-only scanner ignores DLL files");
    expect(result->pendingFiles.size() == 4, "overlapping roots do not duplicate candidates");

    expect(containingBundleOrSelf(innerBinary.getParentDirectory()) == bundle,
           "root inside bundle resolves to the containing bundle");
    expect(pathKey(single) == pathKey(juce::File(single.getFullPathName().toUpperCase())),
           "deduplication key ignores Windows path case");
}

void testRememberedFolders(const Fixture& fixture)
{
    const auto folder = fixture.directory("remembered/vendor");
    const auto single = fixture.file("remembered/vendor/Selected.vst3");
    const auto bundle = fixture.directory("remembered/vendor/Bundle.vst3");
    const auto inner = fixture.file("remembered/vendor/Bundle.vst3/Contents/x86_64-win/Module.vst3");
    const auto genericFile = fixture.file("remembered/vendor/Selected.dll");
    expect(normaliseInitialFolder(folder) == folder, "existing last folder stays selected");
    expect(normaliseInitialFolder(single) == folder, "single-file plugin uses parent folder");
    expect(normaliseInitialFolder(bundle) == folder, "bundle chooser starts in its parent folder");
    expect(normaliseInitialFolder(inner) == folder, "bundle's inner module uses outer parent folder");
    expect(normaliseInitialFolder(genericFile) == folder, "existing generic file uses parent folder");
    expect(normaliseInitialFolder(folder.getChildFile("removed/subfolder")) == folder,
           "missing last folder falls back to its nearest existing ancestor");
    expect(normaliseInitialFolder(juce::File{})
               == juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
           "empty last folder falls back to Documents");
}

void testCancellationAndBudgets(const Fixture& fixture)
{
    const auto folder = fixture.directory("limited");
    fixture.file("limited/A.vst3");
    fixture.file("limited/B.vst3");
    fixture.file("limited/C.vst3");
    const auto cancelled = std::make_shared<ScanState>();
    cancelled->cancelled.store(true, std::memory_order_relaxed);
    discoverFiles(cancelled, { folder.getFullPathName() });
    expect(cancelled->finished.load(std::memory_order_acquire), "cancelled worker finishes");
    expect(cancelled->pendingFiles.empty() && cancelled->visitedEntries == 0,
           "pre-cancelled scan touches no directory entries");
    expect(!cancelled->limitReached, "cancellation is not misreported as a scan limit");

    ScanLimits entries;
    entries.maximumEntries = 2;
    const auto entryResult = scanRoots({ folder.getFullPathName() }, entries);
    expect(entryResult->limitReached, "entry-count limit is reported");
    expect(entryResult->visitedEntries == 2, "entry-count limit is not exceeded");

    ScanLimits directories;
    directories.maximumDirectories = 0;
    const auto directoryResult = scanRoots({ folder.getFullPathName() }, directories);
    expect(directoryResult->limitReached && directoryResult->visitedEntries == 0,
           "zero directory budget finishes without traversal");

    ScanLimits time;
    time.timeBudget = std::chrono::milliseconds(0);
    const auto timeResult = scanRoots({ folder.getFullPathName() }, time);
    expect(timeResult->limitReached && timeResult->visitedEntries == 0,
           "expired time budget finishes without traversal");

    const auto depthRoot = fixture.directory("depth");
    const auto shallow = fixture.file("depth/one/Shallow.vst3");
    const auto deep = fixture.file("depth/one/two/Deep.vst3");
    ScanLimits depth;
    depth.maximumDepth = 1;
    const auto depthResult = scanRoots({ depthRoot.getFullPathName() }, depth);
    expect(depthResult->limitReached, "depth limit is reported");
    expect(contains(*depthResult, shallow) && !contains(*depthResult, deep),
           "depth limit preserves shallow matches and excludes deeper entries");
}

bool createDirectoryLink(const juce::File& source, const juce::File& link)
{
    if (source.createSymbolicLink(link, false))
        return true;

#if JUCE_WINDOWS
    // Creating a directory junction normally does not need the Windows
    // symlink privilege. Its target and link both remain inside our fixture.
    if (link.exists() || link.createDirectory().failed())
        return false;
    const auto handle = CreateFileW(link.getFullPathName().toWideCharPointer(),
                                    GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    const std::wstring printPath(source.getFullPathName().toWideCharPointer());
    const std::wstring substitutePath = L"\\??\\" + printPath;
    const auto substituteBytes = substitutePath.size() * sizeof(wchar_t);
    const auto printBytes = printPath.size() * sizeof(wchar_t);
    struct JunctionHeader
    {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
    };
    static_assert(sizeof(JunctionHeader) == 16);
    std::vector<std::byte> buffer(sizeof(JunctionHeader) + substituteBytes + printBytes
                                  + 2 * sizeof(wchar_t));
    const JunctionHeader header {
        IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(buffer.size() - 8), 0,
        0, static_cast<WORD>(substituteBytes),
        static_cast<WORD>(substituteBytes + sizeof(wchar_t)), static_cast<WORD>(printBytes)
    };
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitutePath.data(), substituteBytes);
    std::memcpy(buffer.data() + sizeof(header) + substituteBytes + sizeof(wchar_t),
                printPath.data(), printBytes);
    DWORD bytesReturned = 0;
    const auto success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT,
                                         buffer.data(), static_cast<DWORD>(buffer.size()),
                                         nullptr, 0, &bytesReturned, nullptr) != FALSE;
    CloseHandle(handle);
    if (!success)
        link.deleteFile();
    return success;
#else
    return false;
#endif
}

void testLinkTraversal(const Fixture& fixture)
{
    const auto source = fixture.directory("link-source");
    const auto linkedPlugin = fixture.file("link-source/nested/Linked.vst3");
    const auto root = fixture.directory("link-root");
    const auto link = root.getChildFile("alias");

    if (!createDirectoryLink(source, link))
    {
        std::cout << "SKIPPED: directory-link fixture (filesystem denied link creation)\n";
        return;
    }

    expect(hasReparseAncestor(link), "link itself is identified as a reparse path");
    expect(hasReparseAncestor(link.getChildFile("nested")), "link ancestor is detected");
    const auto result = scanRoots({ root.getFullPathName(), link.getFullPathName(),
                                    link.getChildFile("nested").getFullPathName() });
    expect(result->pendingFiles.empty(), "neither discovered nor root links are traversed");
    expect(linkedPlugin.existsAsFile(), "scan did not alter the link target");

    // Remove the link itself before the fixture's ordinary recursive cleanup.
    expect(link.deleteFile(), "isolated test symlink is removed safely");
}

void testDefaultRoots()
{
    const auto roots = makeDefaultSearchRoots();
    expect(!roots.isEmpty(), "environment-based default search roots are available");
    std::unordered_set<std::string> unique;
    for (const auto& root : roots)
    {
        expect(juce::File::isAbsolutePath(root), "default root is an absolute path");
        expect(unique.insert(pathKey(juce::File(root))).second,
               "default roots are case-insensitively deduplicated");
    }

    const auto systemDrive = juce::SystemStats::getEnvironmentVariable("SystemDrive", {});
    if (systemDrive.isNotEmpty())
    {
        const auto expected = juce::File(systemDrive + "\\").getChildFile("VST3");
        expect(roots.contains(expected.getFullPathName(), true),
               "system drive VST3 folder is included without a hardcoded username");
    }
}
} // namespace

int main()
{
    try
    {
        const Fixture fixture;
        testDiscoveryAndDeduplication(fixture);
        testRememberedFolders(fixture);
        testCancellationAndBudgets(fixture);
        testLinkTraversal(fixture);
        testDefaultRoots();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Discovery fixture failure: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Plugin discovery: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
