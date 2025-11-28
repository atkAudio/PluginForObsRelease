#pragma once

#include "../../QtParentedWindow.h"
#include "../Core/HostAudioProcessor.h"
#include "PluginHostFooter.h"
#include "UICommon.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServerSettingsComponent.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServerSettingsComponent.h>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>

/**
 * UI component that wraps a loaded plugin's editor with additional controls.
 */
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

        // Set up Multi toggle callbacks
        if (processor && processor->getMultiCoreEnabled && processor->setMultiCoreEnabled)
            footer.setMultiCoreCallbacks(processor->getMultiCoreEnabled, processor->setMultiCoreEnabled);
    }

    void setScaleFactor(float scale)
    {
        if (editor != nullptr)
            editor->setScaleFactor(scale);
    }

    void resized() override
    {
        doLayout(editor.get(), footer, buttonHeight, getLocalBounds());
    }

    void childBoundsChanged(juce::Component* child) override
    {
        if (child != editor.get())
            return;

        const auto size = editor != nullptr ? editor->getLocalBounds() : juce::Rectangle<int>();
        setSize(size.getWidth(), margin + buttonHeight + size.getHeight());
    }

private:
    void showAudioWindow()
    {
        if (audioWindow == nullptr)
        {
            class AudioSettingsWindow : public atk::QtParentedDocumentWindow
            {
            public:
                AudioSettingsWindow(HostAudioProcessorImpl* hostProc)
                    : atk::QtParentedDocumentWindow(
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

                        // Get INPUT channel names
                        int numInputChannels = innerPlugin->getTotalNumInputChannels();
                        for (int i = 0; i < numInputChannels; ++i)
                        {
                            juce::String channelName;

                            // Try to find which bus this channel belongs to
                            bool found = false;
                            for (int bus = 0; bus < innerPlugin->getBusCount(true); ++bus)
                            {
                                auto* busPtr = innerPlugin->getBus(true, bus);
                                if (busPtr)
                                {
                                    int busStartChannel = busPtr->getChannelIndexInProcessBlockBuffer(0);
                                    int busChannelCount = busPtr->getNumberOfChannels();

                                    if (i >= busStartChannel && i < busStartChannel + busChannelCount)
                                    {
                                        int channelInBus = i - busStartChannel;
                                        auto layout = busPtr->getCurrentLayout();
                                        auto channelType = layout.getTypeOfChannel(channelInBus);
                                        auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);

                                        // Include bus name if not the main bus
                                        auto busName = busPtr->getName();
                                        if (busName.isNotEmpty() && busName != "Input" && busName != "Output")
                                            channelName = channelTypeName + " " + busName;
                                        else
                                            channelName = channelTypeName;

                                        found = true;
                                        break;
                                    }
                                }
                            }

                            if (!found || channelName.isEmpty())
                                channelName = "In " + juce::String(i + 1);

                            inputChannelNames.add(channelName);
                        }

                        // Get OUTPUT channel names
                        int numOutputChannels = innerPlugin->getTotalNumOutputChannels();
                        for (int i = 0; i < numOutputChannels; ++i)
                        {
                            juce::String channelName;

                            // Try to find which bus this channel belongs to
                            bool found = false;
                            for (int bus = 0; bus < innerPlugin->getBusCount(false); ++bus)
                            {
                                auto* busPtr = innerPlugin->getBus(false, bus);
                                if (busPtr)
                                {
                                    int busStartChannel = busPtr->getChannelIndexInProcessBlockBuffer(0);
                                    int busChannelCount = busPtr->getNumberOfChannels();

                                    if (i >= busStartChannel && i < busStartChannel + busChannelCount)
                                    {
                                        int channelInBus = i - busStartChannel;
                                        auto layout = busPtr->getCurrentLayout();
                                        auto channelType = layout.getTypeOfChannel(channelInBus);
                                        auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);

                                        // Include bus name if not the main bus
                                        auto busName = busPtr->getName();
                                        if (busName.isNotEmpty() && busName != "Input" && busName != "Output")
                                            channelName = channelTypeName + " " + busName;
                                        else
                                            channelName = channelTypeName;

                                        found = true;
                                        break;
                                    }
                                }
                            }

                            if (!found || channelName.isEmpty())
                                channelName = "Out " + juce::String(i + 1);

                            outputChannelNames.add(channelName);
                        }

                        // Set up OBS channel names (from HostAudioProcessor's channels)
                        juce::StringArray obsInputChannelNames;
                        juce::StringArray obsOutputChannelNames;

                        int numMainChannels = hostProc->getBus(true, 0)->getNumberOfChannels();

                        // Add main OBS input channels
                        for (int i = 0; i < numMainChannels; ++i)
                        {
                            auto layout = hostProc->getBus(true, 0)->getCurrentLayout();
                            auto channelType = layout.getTypeOfChannel(i);
                            auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                            obsInputChannelNames.add("OBS " + channelTypeName);
                        }

                        // Add sidechain channels only if sidechain is enabled in OBS
                        if (hostProc->isSidechainEnabled())
                        {
                            // Use the same channel layout as main channels
                            auto mainLayout = hostProc->getBus(true, 0)->getCurrentLayout();

                            for (int i = 0; i < numMainChannels; ++i)
                            {
                                auto channelType = mainLayout.getTypeOfChannel(i);
                                auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                                obsInputChannelNames.add("OBS Sidechain " + channelTypeName);
                            }
                        }

                        // Output channels - only main bus (no sidechain for outputs)
                        int numObsOutputChannels = hostProc->getBus(false, 0)->getNumberOfChannels();
                        for (int i = 0; i < numObsOutputChannels; ++i)
                        {
                            auto layout = hostProc->getBus(false, 0)->getCurrentLayout();
                            auto channelType = layout.getTypeOfChannel(i);
                            auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                            obsOutputChannelNames.add("OBS " + channelTypeName);
                        }

                        // Set fixed top rows for OBS channels on both input and output matrices
                        audioComponent->setInputFixedTopRows(obsInputChannelNames, true);
                        audioComponent->setOutputFixedTopRows(obsOutputChannelNames, true);

                        // Restore current OBS channel mappings AND device subscriptions from processor
                        // Use setCompleteRoutingMatrices to restore ALL rows (not just fixed OBS rows)
                        auto currentInputMapping = hostProc->getInputChannelMapping();
                        auto currentOutputMapping = hostProc->getOutputChannelMapping();
                        if (!currentInputMapping.empty() && !currentOutputMapping.empty())
                            audioComponent->setCompleteRoutingMatrices(currentInputMapping, currentOutputMapping);

                        audioComponent
                            ->setClientChannelInfo(inputChannelNames, outputChannelNames, innerPlugin->getName());
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
            class MidiSettingsWindow : public atk::QtParentedDocumentWindow
            {
            public:
                MidiSettingsWindow(atk::MidiClient* client)
                    : atk::QtParentedDocumentWindow(
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

    static constexpr auto buttonHeight = 40;

    HostAudioProcessorImpl* processor = nullptr;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    PluginHostFooter footer;
    std::unique_ptr<juce::DocumentWindow> audioWindow;
    std::unique_ptr<juce::DocumentWindow> midiWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorComponent)
};
