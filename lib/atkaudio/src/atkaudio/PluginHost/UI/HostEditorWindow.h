#pragma once

#include "../../LookAndFeel.h"
#include "../Core/HostAudioProcessor.h"
#include "../Core/PluginHolder.h"
#include "PluginEditorComponent.h"
#include "PluginLoaderComponent.h"
#include "UICommon.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

/**
 * Main editor for the HostAudioProcessor.
 * Shows either the plugin loader or the loaded plugin's editor.
 */
class HostAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit HostAudioProcessorEditor(HostAudioProcessorImpl& owner);

    void parentSizeChanged() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void childBoundsChanged(juce::Component* child) override;
    void setScaleFactor(float scale) override;

private:
    void pluginChanged();
    void clearPlugin();

    class ScaledDocumentWindow final : public juce::DocumentWindow
    {
    public:
        ScaledDocumentWindow(juce::Colour bg, float scale)
            : juce::DocumentWindow("Editor", bg, 0)
            , desktopScale(scale)
        {
        }

        float getDesktopScaleFactor() const override
        {
            return juce::Desktop::getInstance().getGlobalScaleFactor() * desktopScale;
        }

    private:
        float desktopScale = 1.0f;
    };

    static constexpr auto buttonHeight = 30;

    HostAudioProcessorImpl& hostProcessor;
    PluginLoaderComponent loader;
    std::unique_ptr<juce::Component> editor;
    PluginEditorComponent* currentEditorComponent = nullptr;
    juce::ScopedValueSetter<std::function<void()>> scopedCallback;
    float currentScaleFactor = 1.0f;
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostAudioProcessorEditor)
};

//==============================================================================
/**
 * Main window for the standalone plugin host.
 * Contains the plugin editor and manages window state.
 */
class HostEditorWindow
    : public juce::DocumentWindow
    , private juce::Button::Listener
{
public:
    using PluginInOuts = PluginHolder::PluginInOuts;

    HostEditorWindow(
        const juce::String& title,
        juce::Colour backgroundColour,
        std::unique_ptr<PluginHolder> pluginHolderIn,
        std::function<bool()> getMultiCoreEnabledCallback = nullptr,
        std::function<void(bool)> setMultiCoreEnabledCallback = nullptr
    );

    ~HostEditorWindow() override;

    void visibilityChanged() override;
    void closeButtonPressed() override;
    void obsPluginShutdown();
    void resized() override;

    juce::AudioProcessor* getAudioProcessor() const noexcept;
    HostAudioProcessorImpl* getHostProcessor() const noexcept;
    juce::CriticalSection& getPluginHolderLock();

    void resetToDefaultState();

    PluginHolder* getPluginHolder();
    std::unique_ptr<PluginHolder> pluginHolder;

private:
    class MainContentComponent;
    class DecoratorConstrainer;

    void updateContent();
    void buttonClicked(juce::Button*) override;
    void handleMenuResult(int result);
    static void menuCallback(int result, HostEditorWindow* button);

    juce::CriticalSection pluginHolderLock;
    DecoratorConstrainer* decoratorConstrainer;

    std::function<bool()> getMultiCoreEnabled;
    std::function<void(bool)> setMultiCoreEnabled;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostEditorWindow)
};
