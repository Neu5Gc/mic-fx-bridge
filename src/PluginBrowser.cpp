#if defined(MIC_VST_BRIDGE_DISCOVERY_TEST) && MIC_VST_BRIDGE_DISCOVERY_TEST
#include <juce_core/juce_core.h>
#else
#include "PluginBrowser.h"
#include "UiLayout.h"
#endif

#include "UiText.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
struct ScanState
{
    std::atomic<bool> cancelled { false };
    std::atomic<bool> finished { false };
    std::mutex mutex;
    std::vector<juce::File> pendingFiles;
    juce::String currentFolder;
    juce::String error;
    int visitedEntries = 0;
    bool limitReached = false;
};

struct ScanLimits
{
    int maximumDirectories = 12000;
    int maximumEntries = 150000;
    int maximumDepth = 32;
    std::chrono::milliseconds timeBudget { 30000 };
};

std::string pathKey(const juce::File& file)
{
    return file.getFullPathName().replaceCharacter('\\', '/').toLowerCase().toStdString();
}

bool isCandidate(const juce::File& file, bool isDirectory)
{
    juce::ignoreUnused(isDirectory);
    return file.hasFileExtension(".vst3");
}

// JUCE's Windows isSymbolicLink() checks FILE_ATTRIBUTE_REPARSE_POINT, which
// includes junctions. Check the ancestors of a supplied root as well as each
// enumerated child, otherwise a custom root could enter a junction indirectly.
bool hasReparseAncestor(juce::File file)
{
    for (;;)
    {
        if (file.isSymbolicLink())
            return true;
        const auto parent = file.getParentDirectory();
        if (parent == file)
            return false;
        file = parent;
    }
}

juce::File containingBundleOrSelf(juce::File file)
{
    auto candidate = file;
    for (;;)
    {
        if (file.hasFileExtension(".vst3"))
            candidate = file;
        const auto parent = file.getParentDirectory();
        if (parent == file)
            return candidate;
        file = parent;
    }
}

juce::File normaliseInitialFolder(juce::File candidate)
{
    candidate = containingBundleOrSelf(candidate);
    if (candidate.hasFileExtension(".vst3") || candidate.existsAsFile())
        candidate = candidate.getParentDirectory();

    while (candidate != juce::File{})
    {
        if (candidate.isDirectory())
            return candidate;
        const auto parent = candidate.getParentDirectory();
        if (parent == candidate)
            break;
        candidate = parent;
    }
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

void discoverFiles(const std::shared_ptr<ScanState>& state,
                   const juce::StringArray& searchRoots,
                   const ScanLimits& limits = {})
{
    // Never wait for this worker from the message/audio thread. It owns only
    // shared scan data, and checks cancellation between filesystem entries.
    // These budgets also bound accidentally selecting an entire local drive.
    const auto deadline = std::chrono::steady_clock::now() + limits.timeBudget;
    int visitedDirectories = 0;
    int visitedEntries = 0;
    bool limitReached = false;

    const auto shouldStop = [&]
    {
        if (state->cancelled.load(std::memory_order_relaxed))
            return true;
        limitReached = visitedDirectories >= limits.maximumDirectories
                       || visitedEntries >= limits.maximumEntries
                       || std::chrono::steady_clock::now() >= deadline;
        return limitReached;
    };

    try
    {
        std::unordered_set<std::string> seenDirectories;
        std::unordered_set<std::string> seenFiles;
        std::deque<std::pair<juce::File, int>> directories;

        const auto addCandidate = [&](const juce::File& file)
        {
            if (seenFiles.insert(pathKey(file)).second)
            {
                const std::lock_guard lock(state->mutex);
                state->pendingFiles.push_back(file);
            }
        };

        for (const auto& root : searchRoots)
        {
            if (shouldStop())
                break;
            if (!juce::File::isAbsolutePath(root))
                continue;

            // Network shares can block inside an OS directory call for a long
            // time. Discovery is intentionally local; direct file selection is
            // still available for unusual installation locations.
            if (root.startsWith("\\\\") || root.startsWith("//"))
                continue;

            const auto directory = containingBundleOrSelf(juce::File(root));
            if (!directory.isOnHardDisk() || hasReparseAncestor(directory)
                || !directory.isDirectory())
                continue;
            if (directory.hasFileExtension(".vst3"))
                addCandidate(directory);
            else
                directories.emplace_back(directory, 0);
        }

        while (!directories.empty() && !shouldStop())
        {
            auto [directory, depth] = std::move(directories.front());
            directories.pop_front();
            if (!seenDirectories.insert(pathKey(directory)).second)
                continue;
            if (directory.isSymbolicLink())
                continue;

            ++visitedDirectories;
            {
                const std::lock_guard lock(state->mutex);
                state->currentFolder = directory.getFullPathName();
                state->visitedEntries = visitedEntries;
            }

            // Iterate one level at a time so bundles are leaves, cancellation
            // is checked at every entry, and directory depth is explicitly capped.
            for (const auto& entry : juce::RangedDirectoryIterator(
                     directory, false, "*", juce::File::findFilesAndDirectories,
                     juce::File::FollowSymlinks::no))
            {
                if (shouldStop())
                    break;
                ++visitedEntries;
                const auto file = entry.getFile();
                if (file.isSymbolicLink())
                    continue;
                if (isCandidate(file, entry.isDirectory()))
                {
                    addCandidate(file);
                    continue;
                }

                if (entry.isDirectory())
                {
                    if (depth < limits.maximumDepth)
                        directories.emplace_back(file, depth + 1);
                    else
                    {
                        const std::lock_guard lock(state->mutex);
                        state->limitReached = true;
                    }
                }
            }
        }
    }
    catch (const std::exception& exception)
    {
        const std::lock_guard lock(state->mutex);
        state->error = mic_daw::uiText(u8"파일 탐색을 완료하지 못했습니다: ",
                                       "Could not complete the file scan: ")
                       + juce::String::fromUTF8(exception.what());
    }
    catch (...)
    {
        const std::lock_guard lock(state->mutex);
        state->error = mic_daw::uiText(u8"파일 탐색 중 오류가 발생했습니다.",
                                       "An error occurred while scanning files.");
    }

    {
        const std::lock_guard lock(state->mutex);
        state->visitedEntries = visitedEntries;
        state->limitReached = state->limitReached || limitReached;
    }
    state->finished.store(true, std::memory_order_release);
}

void addEnvironmentRoot(juce::StringArray& roots,
                        const juce::String& variable,
                        const juce::String& suffix)
{
    auto base = juce::SystemStats::getEnvironmentVariable(variable, {});
    if (base.length() == 2 && base[1] == ':')
        base += "\\";
    if (base.isNotEmpty() && juce::File::isAbsolutePath(base))
        roots.addIfNotAlreadyThere(juce::File(base).getChildFile(suffix).getFullPathName(), true);
}

juce::StringArray makeDefaultSearchRoots()
{
    juce::StringArray roots;
    addEnvironmentRoot(roots, "CommonProgramW6432", "VST3");
    addEnvironmentRoot(roots, "CommonProgramFiles", "VST3");
    addEnvironmentRoot(roots, "LOCALAPPDATA", "Programs/Common/VST3");
    addEnvironmentRoot(roots, "SystemDrive", "VST3");

    return roots;
}
} // namespace

#if !defined(MIC_VST_BRIDGE_DISCOVERY_TEST) || !MIC_VST_BRIDGE_DISCOVERY_TEST

//==============================================================================
class PluginBrowserWindow::Content final : public juce::Component,
                                            private juce::ListBoxModel,
                                            private juce::Timer
{
public:
    Content(PluginBrowserWindow& windowToUse, juce::File initialFolder,
            juce::StringArray extraRoots)
        : window(windowToUse), lastFolder(std::move(initialFolder)),
          roots(PluginBrowserWindow::defaultSearchRoots()),
          tooltipWindow(this, 600)
    {
        for (const auto& root : extraRoots)
            if (juce::File::isAbsolutePath(root))
                roots.addIfNotAlreadyThere(juce::File(root).getFullPathName(), true);

        heading.setText(mic_daw::uiText(u8"발견된 VST 파일", "Discovered VST Files"),
                        juce::dontSendNotification);
        heading.setFont(juce::Font(juce::FontOptions(21.0f).withStyle("Bold")));
        explanation.setText(mic_daw::uiText(
                                u8"파일 이름과 경로를 검색합니다. 실제 플러그인 검증은 불러올 때 진행합니다.",
                                "Search by file name or path. Plug-ins are validated when they are loaded."),
                            juce::dontSendNotification);
        explanation.setColour(juce::Label::textColourId, juce::Colour(0xffaab6c8));

        search.setTextToShowWhenEmpty(mic_daw::uiText(
                                          u8"이름 또는 폴더로 검색",
                                          "Search by name or folder"),
                                      juce::Colour(0xff8899af));
        search.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff151e2b));
        search.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff354258));
        search.setColour(juce::TextEditor::textColourId, juce::Colour(0xffe6edf7));
        search.onTextChange = [this] { updateFilter(); };
        search.onReturnKey = [this] { chooseSelected(); };
        search.onEscapeKey = [this] { window.closeButtonPressed(); };

        list.setModel(this);
        list.setRowHeight(52);
        list.setMultipleSelectionEnabled(false);
        list.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff101823));
        list.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff354258));
        list.setOutlineThickness(1);
        list.setName(mic_daw::uiText(u8"발견된 VST 파일 목록", "Discovered VST file list"));
        emptyMessage.setJustificationType(juce::Justification::centred);
        emptyMessage.setColour(juce::Label::textColourId, juce::Colour(0xff9cabbe));
        emptyMessage.setInterceptsMouseClicks(false, false);

        rootSummary.setColour(juce::Label::textColourId, juce::Colour(0xff92a8c6));
        status.setColour(juce::Label::textColourId, juce::Colour(0xffaab6c8));
        selectedPath.setColour(juce::Label::textColourId, juce::Colour(0xff9cabbe));
        selectedPath.setMinimumHorizontalScale(1.0f);
        selectedPath.setFont(juce::Font(juce::FontOptions(12.0f)));

        rescan.onClick = [this] { requestScan(); };
        addFolder.onClick = [this] { chooseFolder(); };
        browse.onClick = [this] { window.browseFile(); };
        choose.onClick = [this] { chooseSelected(); };
        cancel.onClick = [this] { window.closeButtonPressed(); };
        choose.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2866b5));
        choose.setEnabled(false);

        for (auto* component : std::initializer_list<juce::Component*> {
                 &heading, &explanation, &search, &rescan, &addFolder, &rootSummary,
                 &list, &emptyMessage, &status, &selectedPath, &browse, &choose, &cancel })
            addAndMakeVisible(component);

        updateRootSummary();
        setSize(880, 610);
        startTimer(150);
        requestScan();
    }

    ~Content() override
    {
        stopTimer();
        stopScan();
        folderChooser.reset();
        list.setModel(nullptr);
    }

    void stopScan()
    {
        rescanPending = false;
        if (scan != nullptr)
            scan->cancelled.store(true, std::memory_order_relaxed);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colour(0xff101923));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(18);
        heading.setBounds(area.removeFromTop(34));
        explanation.setBounds(area.removeFromTop(27));
        area.removeFromTop(10);
        auto searchRow = area.removeFromTop(mic_daw::standardButtonHeight);
        addFolder.setBounds(searchRow.removeFromRight(mic_daw::standardButtonWidth));
        searchRow.removeFromRight(8);
        rescan.setBounds(searchRow.removeFromRight(mic_daw::standardButtonWidth));
        searchRow.removeFromRight(10);
        search.setBounds(searchRow);
        rootSummary.setBounds(area.removeFromTop(28));

        auto buttons = area.removeFromBottom(mic_daw::standardButtonHeight);
        cancel.setBounds(buttons.removeFromRight(mic_daw::standardButtonWidth));
        buttons.removeFromRight(8);
        choose.setBounds(buttons.removeFromRight(mic_daw::standardButtonWidth));
        browse.setBounds(buttons.removeFromLeft(mic_daw::standardButtonWidth));
        area.removeFromBottom(8);
        status.setBounds(area.removeFromBottom(25));
        selectedPath.setBounds(area.removeFromBottom(23));
        area.removeFromBottom(5);
        list.setBounds(area);
        emptyMessage.setBounds(area.reduced(16));
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            window.closeButtonPressed();
            return true;
        }
        return false;
    }

private:
    int getNumRows() override { return static_cast<int>(filtered.size()); }

    const juce::File* fileForRow(int row) const
    {
        if (!juce::isPositiveAndBelow(row, static_cast<int>(filtered.size())))
            return nullptr;
        return &files[static_cast<std::size_t>(filtered[static_cast<std::size_t>(row)])];
    }

    void paintListBoxItem(int row, juce::Graphics& graphics,
                          int width, int height, bool selected) override
    {
        const auto* file = fileForRow(row);
        if (file == nullptr)
            return;

        if (selected)
            graphics.fillAll(juce::Colour(0xff284a72));
        else if (row % 2 != 0)
            graphics.fillAll(juce::Colour(0xff141f2d));

        auto area = juce::Rectangle<int>(width, height).reduced(11, 5);
        auto titleRow = area.removeFromTop(23);
        graphics.setColour(juce::Colour(0xff94b9e8));
        graphics.setFont(juce::Font(juce::FontOptions(11.5f)));
        graphics.drawText("VST3",
                           titleRow.removeFromRight(55), juce::Justification::centredRight);
        graphics.setColour(juce::Colour(0xffe6edf7));
        graphics.setFont(juce::Font(juce::FontOptions(14.5f)));
        graphics.drawText(file->getFileNameWithoutExtension(), titleRow,
                           juce::Justification::centredLeft, true);
        graphics.setColour(juce::Colour(0xffa2b3c8));
        graphics.setFont(juce::Font(juce::FontOptions(11.5f)));
        graphics.drawText(file->getParentDirectory().getFullPathName(), area,
                           juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged(int) override
    {
        const auto* file = fileForRow(list.getSelectedRow());
        choose.setEnabled(file != nullptr);
        selectedPath.setText(file != nullptr ? file->getFullPathName() : juce::String{},
                              juce::dontSendNotification);
        selectedPath.setTooltip(file != nullptr ? file->getFullPathName() : juce::String{});
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (const auto* file = fileForRow(row))
            window.pickFile(*file);
    }

    void returnKeyPressed(int) override { chooseSelected(); }

    juce::String getTooltipForRow(int row) override
    {
        const auto* file = fileForRow(row);
        return file != nullptr ? file->getFullPathName() : juce::String{};
    }

    void chooseSelected()
    {
        if (const auto* file = fileForRow(list.getSelectedRow()))
            window.pickFile(*file);
    }

    void updateRootSummary()
    {
        rootSummary.setText(mic_daw::uiText(u8"탐색 위치 ", "Search locations: ")
                            + juce::String(roots.size())
                            + mic_daw::uiText(
                                u8"곳 · 하위 폴더 포함 · 링크/네트워크/이동식 경로 제외",
                                " (including subfolders; links, network paths, and removable drives excluded)"),
                            juce::dontSendNotification);
        rootSummary.setTooltip(roots.joinIntoString("\n"));
    }

    void updateFilter()
    {
        const auto* selectedFile = fileForRow(list.getSelectedRow());
        const auto previousSelection = selectedFile != nullptr ? *selectedFile : juce::File{};
        const auto tokens = juce::StringArray::fromTokens(search.getText().trim(), true);
        filtered.clear();
        int selectedRow = -1;
        for (std::size_t index = 0; index < files.size(); ++index)
        {
            const auto path = files[index].getFullPathName();
            const auto matches = std::all_of(tokens.begin(), tokens.end(), [&](const auto& token)
            {
                return path.containsIgnoreCase(token.unquoted());
            });
            if (matches)
            {
                if (files[index] == previousSelection)
                    selectedRow = static_cast<int>(filtered.size());
                filtered.push_back(static_cast<int>(index));
            }
        }

        list.updateContent();
        if (selectedRow >= 0)
            list.selectRow(selectedRow, true);
        else if (!filtered.empty())
            list.selectRow(0);
        else
            list.deselectAllRows();

        selectedRowsChanged(list.getSelectedRow());
        emptyMessage.setVisible(filtered.empty());
        updateStatus();
        list.repaint();
    }

    void requestScan()
    {
        if (scan != nullptr && !scan->finished.load(std::memory_order_acquire))
        {
            scan->cancelled.store(true, std::memory_order_relaxed);
            rescanPending = true;
            updateStatus();
            return;
        }

        rescanPending = false;
        scan = std::make_shared<ScanState>();
        files.clear();
        filtered.clear();
        limited = false;
        scanError.clear();
        activeFolder.clear();
        scannedEntries = 0;
        scanning = true;
        updateFilter();

        try
        {
            // A detached worker cannot reference this window or its callbacks.
            // Closing the browser is immediate even if an OS file call is slow.
            std::thread([state = scan, searchRoots = roots]
            {
                discoverFiles(state, searchRoots);
            }).detach();
        }
        catch (const std::exception&)
        {
            scan->finished.store(true, std::memory_order_release);
            scanning = false;
            scanError = mic_daw::uiText(
                u8"탐색 작업을 시작하지 못했습니다. 다시 탐색을 눌러 주세요.",
                "Could not start the scan. Select Rescan to try again.");
            updateStatus();
        }
    }

    void timerCallback() override
    {
        if (scan == nullptr)
            return;

        const auto finished = scan->finished.load(std::memory_order_acquire);
        std::vector<juce::File> additions;
        {
            const std::lock_guard lock(scan->mutex);
            additions.swap(scan->pendingFiles);
            activeFolder = scan->currentFolder;
            scannedEntries = scan->visitedEntries;
            limited = scan->limitReached;
            if (scan->error.isNotEmpty())
                scanError = scan->error;
        }

        scanning = !finished;
        if (!additions.empty())
        {
            // Keep the previous row's identity while sorting incremental results.
            const auto* selectedFile = fileForRow(list.getSelectedRow());
            const auto previousSelection = selectedFile != nullptr ? *selectedFile : juce::File{};
            files.insert(files.end(), additions.begin(), additions.end());
            std::sort(files.begin(), files.end(), [](const auto& lhs, const auto& rhs)
            {
                const auto comparison = lhs.getFileNameWithoutExtension().compareIgnoreCase(
                    rhs.getFileNameWithoutExtension());
                return comparison != 0 ? comparison < 0
                                       : lhs.getFullPathName().compareIgnoreCase(rhs.getFullPathName()) < 0;
            });
            filtered.clear();
            list.deselectAllRows();
            updateFilter();
            for (std::size_t row = 0; row < filtered.size(); ++row)
            {
                if (files[static_cast<std::size_t>(filtered[row])] == previousSelection)
                {
                    list.selectRow(static_cast<int>(row), true);
                    break;
                }
            }
        }

        if (finished && rescanPending)
        {
            requestScan();
            return;
        }
        updateStatus();
    }

    void updateStatus()
    {
        auto message = mic_daw::uiText(u8"표시 ", "Showing ")
                       + juce::String(filtered.size())
                       + mic_daw::uiText(u8" / 발견 ", " / found ")
                       + juce::String(files.size())
                       + mic_daw::uiText(u8"개", "");
        if (rescanPending)
            message += mic_daw::uiText(u8" · 다시 탐색 준비 중...",
                                       " - preparing to rescan...");
        else if (scanning)
            message += mic_daw::uiText(u8" · 탐색 중...", " - scanning...");
        else if (scanError.isNotEmpty())
            message += mic_daw::uiText(
                u8" · 탐색 오류 (자세한 내용: 마우스를 올리세요)",
                " - scan error (hover for details)");
        else if (limited)
            message += mic_daw::uiText(
                u8" · 탐색 한도 도달: 더 좁은 폴더를 추가하세요",
                " - scan limit reached; add a more specific folder");
        else
            message += mic_daw::uiText(u8" · 탐색 완료", " - scan complete");

        status.setText(message, juce::dontSendNotification);
        status.setTooltip(scanError.isNotEmpty() ? scanError
                          : mic_daw::uiText(u8"확인한 항목 ", "Entries checked: ")
                            + juce::String(scannedEntries) + "\n" + activeFolder);
        emptyMessage.setText(scanning
                                 ? mic_daw::uiText(u8"VST 파일을 찾는 중입니다...",
                                                   "Searching for VST files...")
                              : files.empty()
                                 ? mic_daw::uiText(
                                       u8"발견된 파일이 없습니다.\n설치 폴더를 추가하거나 파일을 직접 선택하세요.",
                                       "No files were found.\nAdd an installation folder or select a file directly.")
                                 : mic_daw::uiText(u8"검색어에 맞는 파일이 없습니다.",
                                                   "No files match your search."),
                              juce::dontSendNotification);
    }

    void chooseFolder()
    {
        if (folderChooser != nullptr)
            return;

        folderChooser = std::make_unique<juce::FileChooser>(
            mic_daw::uiText(u8"VST 탐색에 추가할 설치 폴더를 선택하세요",
                            "Select an installation folder to add to the VST scan"),
            normaliseInitialFolder(lastFolder), "*");
        addFolder.setEnabled(false);
        folderChooser->launchAsync(juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectDirectories,
                                   [safe = juce::Component::SafePointer<Content>(this)](
                                       const juce::FileChooser& chooser)
        {
            const auto selectedFolder = chooser.getResult();
            if (safe == nullptr)
                return;
            safe->folderChooser.reset();
            safe->addFolder.setEnabled(true);
            if (selectedFolder == juce::File{} || !selectedFolder.isDirectory())
                return;

            if (!selectedFolder.isOnHardDisk() || hasReparseAncestor(selectedFolder))
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    mic_daw::uiText(u8"이 폴더는 자동 탐색에서 제외됩니다",
                                    "This folder cannot be scanned automatically"),
                    mic_daw::uiText(
                        u8"자동 탐색은 로컬 고정 디스크의 일반 폴더를 대상으로 합니다.\n링크, 네트워크, 이동식 디스크의 플러그인은 '파일 직접 선택'으로 불러오세요.",
                        "Automatic scanning supports regular folders on local fixed drives.\nUse 'Select File' to load a plug-in from a link, network path, or removable drive."),
                    mic_daw::uiText(u8"확인", "OK"), safe.getComponent());
                return;
            }

            safe->lastFolder = selectedFolder;
            safe->roots.addIfNotAlreadyThere(selectedFolder.getFullPathName(), true);
            safe->updateRootSummary();
            safe->requestScan();
            const auto callback = safe->window.callbacks.onFolderAdded;
            if (callback)
                callback(selectedFolder);
        });
    }

    PluginBrowserWindow& window;
    juce::File lastFolder;
    juce::StringArray roots;
    std::shared_ptr<ScanState> scan;
    std::vector<juce::File> files;
    std::vector<int> filtered;
    juce::String activeFolder;
    juce::String scanError;
    int scannedEntries = 0;
    bool scanning = false;
    bool limited = false;
    bool rescanPending = false;

    juce::Label heading;
    juce::Label explanation;
    juce::TextEditor search;
    juce::Label rootSummary;
    juce::ListBox list;
    juce::Label emptyMessage;
    juce::Label status;
    juce::Label selectedPath;
    juce::TextButton rescan { mic_daw::uiText(u8"다시 탐색", "Rescan") };
    juce::TextButton addFolder { mic_daw::uiText(u8"폴더 추가", "Add Folder") };
    juce::TextButton browse { mic_daw::uiText(u8"파일 직접 선택", "Select File") };
    juce::TextButton choose { mic_daw::uiText(u8"선택한 VST 불러오기", "Load Selected VST") };
    juce::TextButton cancel { mic_daw::uiText(u8"닫기", "Close") };
    juce::TooltipWindow tooltipWindow;
    std::unique_ptr<juce::FileChooser> folderChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Content)
};

//==============================================================================
juce::StringArray PluginBrowserWindow::defaultSearchRoots()
{
    return makeDefaultSearchRoots();
}

PluginBrowserWindow::PluginBrowserWindow(juce::File initialLastFolder,
                                           juce::StringArray extraSearchRoots,
                                           Callbacks callbacksToUse,
                                           juce::LookAndFeel* lookAndFeelToUse)
    : juce::DocumentWindow(mic_daw::uiText(u8"VST 탐색", "VST Browser"),
                            juce::Colour(0xff101923),
                            juce::DocumentWindow::closeButton),
      callbacks(std::move(callbacksToUse))
{
    setLookAndFeel(lookAndFeelToUse);
    setUsingNativeTitleBar(true);
    setContentOwned(new Content(*this, std::move(initialLastFolder),
                                 std::move(extraSearchRoots)), true);
    setResizable(true, false);
    setResizeLimits(720, 450, 1600, 1100);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
    toFront(true);
}

PluginBrowserWindow::~PluginBrowserWindow()
{
    clearContentComponent();
    setLookAndFeel(nullptr);
}

void PluginBrowserWindow::pickFile(const juce::File& file)
{
    complete([callback = callbacks.onPick, file]
    {
        if (callback)
            callback(file);
    });
}

void PluginBrowserWindow::browseFile()
{
    complete(callbacks.onBrowseFile);
}

void PluginBrowserWindow::closeButtonPressed()
{
    complete(callbacks.onClose);
}

void PluginBrowserWindow::complete(std::function<void()> callback)
{
    if (completing)
        return;
    completing = true;
    if (auto* content = dynamic_cast<Content*>(getContentComponent()))
        content->stopScan();
    setVisible(false);
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PluginBrowserWindow>(this),
                                    callback = std::move(callback)]
    {
        if (safe != nullptr && callback)
            callback();
    });
}

#endif
