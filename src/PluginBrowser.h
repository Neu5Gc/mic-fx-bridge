#pragma once

#include <JuceHeader.h>

#include <functional>

// This browser discovers filenames only. Loading/validating a plugin remains the
// host's responsibility, so enumerating an unknown DLL never executes its code.
class PluginBrowserWindow final : public juce::DocumentWindow
{
public:
    struct Callbacks
    {
        // These three completion callbacks run asynchronously after the window
        // is hidden. They may reset the unique_ptr that owns this window.
        std::function<void(const juce::File&)> onPick;
        std::function<void()> onBrowseFile;
        std::function<void()> onClose;

        // Called on the message thread after selecting a valid search folder.
        // Persist this folder as both an extra search root and the last folder.
        std::function<void(const juce::File&)> onFolderAdded;
    };

    PluginBrowserWindow(juce::File initialLastFolder,
                        juce::StringArray extraSearchRoots,
                        Callbacks callbacksToUse,
                        juce::LookAndFeel* lookAndFeelToUse = nullptr);
    ~PluginBrowserWindow() override;

    void closeButtonPressed() override;

    [[nodiscard]] static juce::StringArray defaultSearchRoots();

private:
    class Content;

    void pickFile(const juce::File& file);
    void browseFile();
    void complete(std::function<void()> callback);

    Callbacks callbacks;
    bool completing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBrowserWindow)
};
