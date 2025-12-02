#pragma once

#include "../Core/HostAudioProcessor.h"
#include "PluginHostFooter.h"
#include "UICommon.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServerSettingsComponent.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServerSettingsComponent.h>
#include <atkaudio/SandboxedPluginScanner.h>
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

        // Use sandboxed scanner (shows warning once if not available)
        auto sandboxedScanner = std::make_unique<atk::SandboxedScanner>();
        if (sandboxedScanner->isScannerAvailable())
            DBG("PluginLoaderComponent: Using sandboxed plugin scanner");
        else
            DBG("PluginLoaderComponent: Sandboxed scanner not available, using in-process scanning");
        list.setCustomScanner(std::move(sandboxedScanner));

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

                    int numMainChannels = processor->getBus(true, 0)->getNumberOfChannels();
                    auto mainLayout = processor->getBus(true, 0)->getCurrentLayout();

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
                    int numOutputChannels = processor->getBus(false, 0)->getNumberOfChannels();
                    auto outputLayout = processor->getBus(false, 0)->getCurrentLayout();
                    for (int i = 0; i < numOutputChannels; ++i)
                    {
                        auto channelType = outputLayout.getTypeOfChannel(i);
                        auto channelTypeName = juce::AudioChannelSet::getChannelTypeName(channelType);
                        obsOutputChannelNames.add("OBS " + channelTypeName);
                    }

                    // Set fixed top rows for OBS channels on both input and output matrices
                    audioSettings->setInputFixedTopRows(obsInputChannelNames, true);
                    audioSettings->setOutputFixedTopRows(obsOutputChannelNames, true);

                    // Set client (plugin) channel info BEFORE setting routing matrices
                    // This ensures the grid has the right column count
                    audioSettings->setClientChannelInfo(inputChannelNames, outputChannelNames, innerPlugin->getName());

                    // Restore current routing matrix from processor
                    auto currentInputMapping = processor->getInputChannelMapping();
                    auto currentOutputMapping = processor->getOutputChannelMapping();
                    if (!currentInputMapping.empty() && !currentOutputMapping.empty())
                        audioSettings->setCompleteRoutingMatrices(currentInputMapping, currentOutputMapping);
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
    }

    void resized() override
    {
        doLayout(&pluginListComponent, footer, 54, getLocalBounds());
    }

private:
    HostAudioProcessorImpl* processor = nullptr;
    juce::PluginListComponent pluginListComponent;
    PluginHostFooter footer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginLoaderComponent)
};
