#include <JuceHeader.h>

#include "MainComponent.h"

class MicVstBridgeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Mic FX Bridge"; }
    const juce::String getApplicationVersion() override { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override
    {
        if (! ownsLegacyInstanceLock)
        {
            const auto legacyMessage = mic_daw::uiText(
                u8"기존 Mic VST Bridge를 종료한 뒤 Mic FX Bridge를 실행하세요.",
                "Close the previous Mic VST Bridge process before starting Mic FX Bridge.");
            auto options = juce::MessageBoxOptions()
                               .withIconType(juce::MessageBoxIconType::WarningIcon)
                               .withTitle(getApplicationName())
                               .withMessage(legacyMessage)
                               .withButton("OK");
            juce::AlertWindow::showAsync(options, [] (int) { juce::JUCEApplicationBase::quit(); });
            return;
        }

        mainWindow = std::make_unique<MainWindow>(getApplicationName() + " " + getApplicationVersion());
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow == nullptr || mainWindow->prepareToQuit())
            quit();
    }

    void anotherInstanceStarted(const juce::String&) override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setVisible(true);
            mainWindow->toFront(true);
        }
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : juce::DocumentWindow(name,
                                   juce::Colour(0xff0c1119),
                                   juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            setResizeLimits(760, 760, 1400, 1100);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        [[nodiscard]] bool prepareToQuit()
        {
            if (auto* content = dynamic_cast<MainComponent*>(getContentComponent()))
                return content->prepareToQuit();
            return true;
        }
    };

    // Keep the previous release's hidden instance lock during the rename. This
    // prevents old and new executables from writing the shared legacy settings
    // file or competing for the same WASAPI endpoints at the same time.
    juce::InterProcessLock legacyInstanceLock { "juceAppLock_Mic VST Bridge" };
    const bool ownsLegacyInstanceLock = legacyInstanceLock.enter(0);
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(MicVstBridgeApplication)
