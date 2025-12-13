#pragma once

#include "../../LookAndFeel.h"

#include "../Core/HostAudioProcessor.h"
#include "../Core/PluginHolder.h"
#include "PluginEditorComponent.h"
#include "PluginLoaderComponent.h"
#include "UICommon.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class HostAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit HostAudioProcessorEditor(HostAudioProcessorImpl& owner);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void childBoundsChanged(juce::Component* child) override;

    void setFooterVisible(bool visible);

    juce::ComponentBoundsConstrainer* getPluginConstrainer() const;

private:
    void pluginChanged();
    void clearPlugin();

    class SimpleDocumentWindow final : public juce::DocumentWindow
    {
    public:
        SimpleDocumentWindow(juce::Colour bg)
            : juce::DocumentWindow("Editor", bg, juce::DocumentWindow::allButtons)
        {
            setTitleBarButtonsRequired(juce::DocumentWindow::closeButton, false);
        }
    };

    HostAudioProcessorImpl& hostProcessor;
    PluginLoaderComponent loader;
    std::unique_ptr<juce::Component> editor;
    PluginEditorComponent* currentEditorComponent = nullptr;
    juce::ScopedValueSetter<std::function<void()>> scopedCallback;
    bool resizingFromChild = false;
    bool pendingFooterVisible = true; // Footer visibility to apply when plugin loads
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostAudioProcessorEditor)
};

class HostEditorComponent final
    : public juce::Component
    , private juce::ComponentListener
{
public:
    HostEditorComponent(std::unique_ptr<PluginHolder> pluginHolderIn);

    ~HostEditorComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void childBoundsChanged(juce::Component* child) override;

    juce::AudioProcessor* getAudioProcessor() const noexcept;
    HostAudioProcessorImpl* getHostProcessor() const noexcept;
    juce::CriticalSection& getPluginHolderLock();

    PluginHolder* getPluginHolder();

    juce::ComponentBoundsConstrainer* getEditorConstrainer() const;

    void destroyUI();
    void recreateUI();

    void setFooterVisible(bool visible);

    void setIsDockedCallback(std::function<bool()> callback)
    {
        getIsDocked = std::move(callback);
    }

    std::unique_ptr<PluginHolder> pluginHolder;

private:
    class MainContentComponent;

    void updateContent();
    void componentMovedOrResized(juce::Component& component, bool wasMoved, bool wasResized) override;

    juce::CriticalSection pluginHolderLock;
    std::unique_ptr<MainContentComponent> contentComponent;
    juce::AudioProcessorEditor* editorToWatch = nullptr;
    bool resizingFromEditor = false;
    std::function<bool()> getIsDocked;

    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE(HostEditorComponent)
};
