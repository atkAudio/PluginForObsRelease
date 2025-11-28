#pragma once

#include "../Core/HostAudioProcessor.h"
#include "PluginHostFooter.h"
#include "UICommon.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServerSettingsComponent.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServerSettingsComponent.h>
#include <functional>
#include <juce_audio_utils/juce_audio_utils.h>

// JUCE utility functions
using juce::isPositiveAndBelow;
using juce::NullCheckedInvocation;

/**
 * UI component for browsing and loading audio plugins.
 * Displays the available plugins and provides controls for loading them.
 */
class PluginLoaderComponent final : public juce::Component
{
public:
    template <typename Callback>
    PluginLoaderComponent(
        juce::AudioPluginFormatManager& manager,
        juce::KnownPluginList& list,
        juce::PropertiesFile* props,
        HostAudioProcessorImpl* processorPtr,
        Callback&& callback
    )
        : pluginListComponent(
              manager,
              list,
              props != nullptr ? props->getFile().getSiblingFile("RecentlyCrashedPluginsList") : juce::File{},
              props,
              false
          )
        , processor(processorPtr)
        , footer("Load plugin", true)
    {
        pluginListComponent.getTableListBox().setMultipleSelectionEnabled(false);

        addAndMakeVisible(pluginListComponent);
        addAndMakeVisible(footer);

        const auto getCallback = [this, &list, cb = std::forward<Callback>(callback)](EditorStyle style)
        {
            return [this, &list, cb, style]
            {
                const auto index = pluginListComponent.getTableListBox().getSelectedRow();
                const auto& types = list.getTypes();

                if (isPositiveAndBelow(index, types.size()))
                    NullCheckedInvocation::invoke(cb, types.getReference(index), style);
            };
        };

        footer.actionButton.onClick = getCallback(EditorStyle::thisWindow);

        footer.audioButton.onClick = [this]
        {
            if (processor)
            {
                auto* audioSettings = new atk::AudioServerSettingsComponent(&processor->audioClient);

                // Configure channel info from inner plugin if loaded
                if (auto* innerPlugin = processor->getInnerPlugin())
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

                    int numMainChannels = processor->getBus(true, 0)->getNumberOfChannels();

                    // Add main OBS input channels
                    for (int i = 0; i < numMainChannels; ++i)
                    {
                        auto layout = processor->getBus(true, 0)->getCurrentLayout();
                        auto channelType = layout.getTypeOfChannel(i);
                        auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                        obsInputChannelNames.add("OBS " + channelTypeName);
                    }

                    // Add sidechain channels only if sidechain is enabled in OBS
                    if (processor->isSidechainEnabled())
                    {
                        // Use the same channel layout as main channels
                        auto mainLayout = processor->getBus(true, 0)->getCurrentLayout();

                        for (int i = 0; i < numMainChannels; ++i)
                        {
                            auto channelType = mainLayout.getTypeOfChannel(i);
                            auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                            obsInputChannelNames.add("OBS Sidechain " + channelTypeName);
                        }
                    }

                    // Output channels - only main bus (no sidechain for outputs)
                    int numObsOutputChannels = processor->getBus(false, 0)->getNumberOfChannels();
                    for (int i = 0; i < numObsOutputChannels; ++i)
                    {
                        auto layout = processor->getBus(false, 0)->getCurrentLayout();
                        auto channelType = layout.getTypeOfChannel(i);
                        auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                        obsOutputChannelNames.add("OBS " + channelTypeName);
                    }

                    // Set fixed top rows for OBS channels on both input and output matrices
                    audioSettings->setInputFixedTopRows(obsInputChannelNames, true);
                    audioSettings->setOutputFixedTopRows(obsOutputChannelNames, true);

                    // Restore current OBS channel mappings from processor if they exist
                    auto currentInputMapping = processor->getInputChannelMapping();
                    auto currentOutputMapping = processor->getOutputChannelMapping();
                    if (!currentInputMapping.empty() && !currentOutputMapping.empty())
                        audioSettings->setObsChannelMappings(currentInputMapping, currentOutputMapping);

                    audioSettings->setClientChannelInfo(inputChannelNames, outputChannelNames, innerPlugin->getName());
                }
                else
                {
                    // No plugin loaded, use default stereo
                    audioSettings->setClientChannelCount(2, "Plugin");
                }

                audioSettings->setSize(900, 700);

                // Set callback to apply OBS channel mapping when user clicks Apply
                audioSettings->onObsMappingChanged = [this](
                                                         const std::vector<std::vector<bool>>& inputMapping,
                                                         const std::vector<std::vector<bool>>& outputMapping
                                                     )
                {
                    if (processor)
                    {
                        processor->setInputChannelMapping(inputMapping);
                        processor->setOutputChannelMapping(outputMapping);
                    }
                };

                // Set callback to get current OBS mappings for Restore button
                audioSettings->getCurrentObsMappings =
                    [this]() -> std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>>
                {
                    if (processor)
                        return {processor->getInputChannelMapping(), processor->getOutputChannelMapping()};
                    return {{}, {}};
                };

                juce::DialogWindow::LaunchOptions options;
                options.content.setOwned(audioSettings);
                options.dialogTitle = "Audio";
                options.dialogBackgroundColour = juce::Colours::darkgrey;
                options.escapeKeyTriggersCloseButton = true;
                options.useNativeTitleBar = false;
                options.resizable = true;
                options.launchAsync();
            }
        };

        footer.midiButton.onClick = [this]
        {
            if (processor)
            {
                auto* midiSettings = new atk::MidiServerSettingsComponent(&processor->midiClient);
                midiSettings->setSize(800, 600);

                juce::DialogWindow::LaunchOptions options;
                options.content.setOwned(midiSettings);
                options.dialogTitle = "MIDI";
                options.dialogBackgroundColour = juce::Colours::darkgrey;
                options.escapeKeyTriggersCloseButton = true;
                options.useNativeTitleBar = false;
                options.resizable = true;
                options.launchAsync();
            }
        };

        // Set up Multi toggle callbacks
        if (processor && processor->getMultiCoreEnabled && processor->setMultiCoreEnabled)
            footer.setMultiCoreCallbacks(processor->getMultiCoreEnabled, processor->setMultiCoreEnabled);
    }

    void resized() override
    {
        doLayout(&pluginListComponent, footer, 40, getLocalBounds());
    }

private:
    HostAudioProcessorImpl* processor = nullptr;
    juce::PluginListComponent pluginListComponent;
    PluginHostFooter footer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginLoaderComponent)
};
