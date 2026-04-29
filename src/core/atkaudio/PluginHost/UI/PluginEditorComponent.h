#pragma once

#include "../Core/HostAudioProcessor.h"
#include "PluginHostFooter.h"
#include "UICommon.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServerSettingsComponent.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServerSettingsComponent.h>
#include <cmath>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class PluginEditorComponent final : public juce::Component
{
public:
    template <typename Callback>
    PluginEditorComponent(
        std::unique_ptr<juce::AudioProcessorEditor> editorIn,
        HostAudioProcessorImpl* processorPtr,
        Callback&& onClose
    )
        : editor(std::move(editorIn))
        , processor(processorPtr)
        , footer("Unload Plugin", true)
    {
        addAndMakeVisible(editor.get());
        addAndMakeVisible(footer);

        childBoundsChanged(editor.get());

        auto lambda = [onClose]
        {
            juce::AlertWindow::showOkCancelBox(
                juce::AlertWindow::WarningIcon,
                "Unload Plugin",
                "Are you sure you want to unload the plugin?",
                "Yes",
                "No",
                nullptr,
                juce::ModalCallbackFunction::create(
                    [onClose](int result)
                    {
                        if (result == 1)
                            onClose();
                    }
                )
            );
        };
        footer.actionButton.onClick = lambda;

        footer.audioButton.onClick = [this] { showAudioWindow(); };
        footer.midiButton.onClick = [this] { showMidiWindow(); };

        // Set up Multi toggle callbacks (use lambdas to read from processor dynamically)
        if (processor)
        {
            footer.setMultiCoreCallbacks(
                [this]() -> bool
                {
                    if (processor && processor->getMultiCoreEnabled)
                        return processor->getMultiCoreEnabled();
                    return false;
                },
                [this](bool enabled)
                {
                    if (processor && processor->setMultiCoreEnabled)
                        processor->setMultiCoreEnabled(enabled);
                }
            );
        }

        // Set up CPU/latency stats callbacks (use lambdas to read from processor dynamically)
        if (processor)
        {
            footer.setStatsCallbacks(
                [this]() -> float
                {
                    if (processor && processor->getCpuLoad)
                        return processor->getCpuLoad();
                    return 0.0f;
                },
                [this]() -> int
                {
                    if (processor && processor->getLatencyMs)
                        return processor->getLatencyMs();
                    return 0;
                }
            );
        }

        // Initialize cached scale factor for DPI change detection
        if (auto* peer = getPeer())
            cachedScaleFactor = (float)peer->getPlatformScaleFactor();
    }

    juce::AudioProcessorEditor* getEditor() const
    {
        return editor.get();
    }

    void setFooterVisible(bool visible)
    {
        if (footer.isVisible() == visible)
            return;

        footer.setVisible(visible);

        // Recalculate size based on footer visibility
        if (editor != nullptr)
        {
            const auto editorSize = editor->getLocalBounds();
            int totalHeight = editorSize.getHeight();
            if (visible)
                totalHeight += margin + buttonHeight;
            resizingFromChild = true;
            setSize(editorSize.getWidth(), totalHeight);
            resizingFromChild = false;

            resized();
            repaint();
        }
    }

    juce::ComponentBoundsConstrainer* getEditorConstrainer() const
    {
        if (editor != nullptr)
        {
            auto* c = editor->getConstrainer();
            if (c != nullptr)
            {
                if (c->getMinimumWidth() <= 0 || c->getMinimumHeight() <= 0)
                    c->setMinimumSize(editor->getWidth(), editor->getHeight());
                return c;
            }
        }
        return nullptr;
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        int footerSpace = footer.isVisible() ? (buttonHeight + margin) : 0;
        auto editorBounds = bounds.removeFromTop(bounds.getHeight() - footerSpace);

        if (footer.isVisible())
        {
            auto footerBounds = bounds.withTrimmedTop(margin);
            footer.setBounds(footerBounds.reduced(margin, 0).withTrimmedBottom(margin));
        }

        if (editor != nullptr && !resizingFromChild)
            editor->setBounds(editorBounds);

        checkScaleFactorChanged();
    }

    void moved() override
    {
        juce::Component::moved();
        checkScaleFactorChanged();
    }

    void childBoundsChanged(juce::Component* child) override
    {
        if (child != editor.get())
            return;

        const auto size = editor != nullptr ? editor->getLocalBounds() : juce::Rectangle<int>();

        int totalHeight = size.getHeight();
        if (footer.isVisible())
            totalHeight += margin + buttonHeight;

        resizingFromChild = true;
        setSize(size.getWidth(), totalHeight);
        resizingFromChild = false;
    }

private:
    void showAudioWindow()
    {
        if (audioWindow == nullptr)
        {
            class AudioSettingsWindow : public juce::DocumentWindow
            {
            public:
                AudioSettingsWindow(HostAudioProcessorImpl* hostProc)
                    : juce::DocumentWindow(
                          "Audio",
                          juce::LookAndFeel::getDefaultLookAndFeel().findColour(
                              juce::ResizableWindow::backgroundColourId
                          ),
                          juce::DocumentWindow::allButtons
                      )
                {
                    setTitleBarButtonsRequired(juce::DocumentWindow::closeButton, false);
                    setResizable(true, false);

                    auto* audioComponent = new atk::AudioServerSettingsComponent(&hostProc->audioClient);

                    // Configure channel info from inner plugin if loaded
                    if (auto* innerPlugin = hostProc->getInnerPlugin())
                    {
                        juce::StringArray inputChannelNames;
                        juce::StringArray outputChannelNames;

                        // Get INPUT channel names from the loaded plugin
                        int numPluginInputs = innerPlugin->getTotalNumInputChannels();
                        for (int i = 0; i < numPluginInputs; ++i)
                        {
                            juce::String channelName;
                            // Find which bus this channel belongs to
                            for (int bus = 0; bus < innerPlugin->getBusCount(true); ++bus)
                            {
                                auto* busPtr = innerPlugin->getBus(true, bus);
                                if (busPtr)
                                {
                                    int busStart = busPtr->getChannelIndexInProcessBlockBuffer(0);
                                    int busEnd = busStart + busPtr->getNumberOfChannels();
                                    if (i >= busStart && i < busEnd)
                                    {
                                        int chInBus = i - busStart;
                                        auto layout = busPtr->getCurrentLayout();
                                        auto channelType = layout.getTypeOfChannel(chInBus);
                                        auto typeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                                        // Add bus name for non-main buses
                                        if (bus == 0)
                                            channelName = typeName;
                                        else
                                            channelName = typeName + " " + busPtr->getName();
                                        break;
                                    }
                                }
                            }
                            if (channelName.isEmpty())
                                channelName = "In " + juce::String(i + 1);
                            inputChannelNames.add(channelName);
                        }

                        // Get OUTPUT channel names from the loaded plugin
                        int numPluginOutputs = innerPlugin->getTotalNumOutputChannels();
                        for (int i = 0; i < numPluginOutputs; ++i)
                        {
                            juce::String channelName;
                            for (int bus = 0; bus < innerPlugin->getBusCount(false); ++bus)
                            {
                                auto* busPtr = innerPlugin->getBus(false, bus);
                                if (busPtr)
                                {
                                    int busStart = busPtr->getChannelIndexInProcessBlockBuffer(0);
                                    int busEnd = busStart + busPtr->getNumberOfChannels();
                                    if (i >= busStart && i < busEnd)
                                    {
                                        int chInBus = i - busStart;
                                        auto layout = busPtr->getCurrentLayout();
                                        auto channelType = layout.getTypeOfChannel(chInBus);
                                        auto typeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                                        if (bus == 0)
                                            channelName = typeName;
                                        else
                                            channelName = typeName + " " + busPtr->getName();
                                        break;
                                    }
                                }
                            }
                            if (channelName.isEmpty())
                                channelName = "Out " + juce::String(i + 1);
                            outputChannelNames.add(channelName);
                        }

                        // Set up OBS channel names (rows) - from HostAudioProcessor's channels
                        juce::StringArray obsInputChannelNames;
                        juce::StringArray obsOutputChannelNames;

                        int numMainChannels = hostProc->getBus(true, 0)->getNumberOfChannels();
                        auto mainLayout = hostProc->getBus(true, 0)->getCurrentLayout();

                        // Add main OBS input channels
                        for (int i = 0; i < numMainChannels; ++i)
                        {
                            auto channelType = mainLayout.getTypeOfChannel(i);
                            auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                            obsInputChannelNames.add("OBS " + channelTypeName);
                        }

                        // Always add sidechain rows - OBS always provides sidechain channels
                        for (int i = 0; i < numMainChannels; ++i)
                        {
                            auto channelType = mainLayout.getTypeOfChannel(i);
                            auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                            obsInputChannelNames.add("OBS Sidechain " + channelTypeName);
                        }

                        // Output channels - only main bus (no sidechain for outputs)
                        int numOutputChannels = hostProc->getBus(false, 0)->getNumberOfChannels();
                        auto outputLayout = hostProc->getBus(false, 0)->getCurrentLayout();
                        for (int i = 0; i < numOutputChannels; ++i)
                        {
                            auto channelType = outputLayout.getTypeOfChannel(i);
                            auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                            obsOutputChannelNames.add("OBS " + channelTypeName);
                        }

                        // Set fixed top rows for OBS channels on both input and output matrices
                        audioComponent->setInputFixedTopRows(obsInputChannelNames, true);
                        audioComponent->setOutputFixedTopRows(obsOutputChannelNames, true);

                        // Set client (plugin) channel info BEFORE setting routing matrices
                        // This ensures the grid has the right column count
                        audioComponent
                            ->setClientChannelInfo(inputChannelNames, outputChannelNames, innerPlugin->getName());

                        // Restore current routing matrix from processor
                        auto currentInputMapping = hostProc->getInputChannelMapping();
                        auto currentOutputMapping = hostProc->getOutputChannelMapping();
                        if (!currentInputMapping.empty() && !currentOutputMapping.empty())
                            audioComponent->setCompleteRoutingMatrices(currentInputMapping, currentOutputMapping);
                    }
                    else
                    {
                        // No plugin loaded, use default stereo
                        audioComponent->setClientChannelCount(2, "Plugin");
                    }

                    // Set callback to apply OBS channel mapping when user clicks Apply
                    audioComponent->onObsMappingChanged = [hostProc](
                                                              const std::vector<std::vector<bool>>& inputMapping,
                                                              const std::vector<std::vector<bool>>& outputMapping
                                                          )
                    {
                        hostProc->setInputChannelMapping(inputMapping);
                        hostProc->setOutputChannelMapping(outputMapping);
                    };

                    // Set callback to get current OBS mappings for Restore button
                    audioComponent->getCurrentObsMappings =
                        [hostProc]() -> std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>>
                    { return {hostProc->getInputChannelMapping(), hostProc->getOutputChannelMapping()}; };

                    setContentOwned(audioComponent, true);

                    centreWithSize(900, 700);
                }

                void closeButtonPressed() override
                {
                    setVisible(false);
                }
            };

            audioWindow = std::make_unique<AudioSettingsWindow>(processor);
            audioWindow->setVisible(true);
        }
        else
        {
            audioWindow->toFront(true);
            audioWindow->setVisible(true);
        }
    }

    void showMidiWindow()
    {
        if (midiWindow == nullptr)
        {
            class MidiSettingsWindow : public juce::DocumentWindow
            {
            public:
                MidiSettingsWindow(atk::MidiClient* client)
                    : juce::DocumentWindow(
                          "MIDI",
                          juce::LookAndFeel::getDefaultLookAndFeel().findColour(
                              juce::ResizableWindow::backgroundColourId
                          ),
                          juce::DocumentWindow::allButtons
                      )
                {
                    setTitleBarButtonsRequired(juce::DocumentWindow::closeButton, false);
                    setResizable(true, false);

                    auto* midiComponent = new atk::MidiServerSettingsComponent(client);
                    setContentOwned(midiComponent, true);

                    centreWithSize(800, 600);
                }

                void closeButtonPressed() override
                {
                    setVisible(false);
                }
            };

            midiWindow = std::make_unique<MidiSettingsWindow>(&processor->midiClient);
            midiWindow->setVisible(true);
        }
        else
        {
            midiWindow->toFront(true);
            midiWindow->setVisible(true);
        }
    }

    static constexpr auto buttonHeight = 54;

    HostAudioProcessorImpl* processor = nullptr;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    PluginHostFooter footer;
    std::unique_ptr<juce::DocumentWindow> audioWindow;
    std::unique_ptr<juce::DocumentWindow> midiWindow;
    bool resizingFromChild = false;
    float cachedScaleFactor = 0.0f;

    // Detect DPI changes when the component is moved or resized.
    // Child windows don't receive WM_DPICHANGED, so we check on movement.
    // When scale changes, we recreate the plugin editor to render at the new DPI.
    void checkScaleFactorChanged()
    {
        if (editor == nullptr || processor == nullptr)
            return;

        if (auto* peer = getPeer())
        {
            float currentScale = (float)peer->getPlatformScaleFactor();
            if (std::abs(currentScale - cachedScaleFactor) > 0.01f)
            {
                DBG("PluginEditorComponent: Scale factor changed from " << cachedScaleFactor << " to " << currentScale);

                // Defer editor recreation to avoid re-entrancy during resize callbacks
                juce::MessageManager::callAsync(
                    [safeThis = juce::Component::SafePointer<PluginEditorComponent>(this)]
                    {
                        if (safeThis != nullptr)
                            safeThis->recreateEditor();
                    }
                );
            }
        }
    }

    void recreateEditor()
    {
        if (processor == nullptr)
            return;

        // Verify scale actually changed (prevents unnecessary recreation from stale/duplicate async calls)
        float newScale = 0.0f;
        if (auto* peer = getPeer())
        {
            newScale = (float)peer->getPlatformScaleFactor();
            if (std::abs(newScale - cachedScaleFactor) < 0.01f)
                return; // Scale hasn't changed, skip recreation
        }

        cachedScaleFactor = newScale;

        // Remove old editor
        if (editor != nullptr)
        {
            removeChildComponent(editor.get());
            if (auto* audioProc = editor->getAudioProcessor())
                audioProc->editorBeingDeleted(editor.get());
            editor.reset();
        }

        // Create new editor
        editor = processor->createInnerEditor();
        if (editor != nullptr)
        {
            addAndMakeVisible(editor.get());
            childBoundsChanged(editor.get());
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorComponent)
};
