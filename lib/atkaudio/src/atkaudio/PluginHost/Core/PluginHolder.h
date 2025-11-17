#pragma once

#include "HostAudioProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>

/**
 * Manages the plugin instance lifecycle for the standalone host.
 * This class handles creation, deletion, and state management of the hosted audio processor.
 */
class PluginHolder
    : private juce::Timer
    , private juce::Value::Listener
{
public:
    struct PluginInOuts
    {
        short numIns, numOuts;
    };

    PluginHolder(
        juce::PropertySet* settingsToUse,
        bool takeOwnershipOfSettings = true,
        const juce::String& preferredDefaultDeviceName = juce::String(),
        const juce::AudioDeviceManager::AudioDeviceSetup* preferredSetupOptions = nullptr,
        const juce::Array<PluginInOuts>& channels = juce::Array<PluginInOuts>(),
        bool shouldAutoOpenMidiDevices = false
    );

    ~PluginHolder() override;

    // Plugin lifecycle
    virtual void createPlugin();
    virtual void deletePlugin();

    // Channel configuration
    int getNumInputChannels() const;
    int getNumOutputChannels() const;

    // Get the inner processor (HostAudioProcessorImpl) for direct access
    HostAudioProcessorImpl* getHostProcessor() const;

    // State management
    void savePluginState();
    void reloadPluginState();
    void askUserToSaveState(const juce::String& fileSuffix = juce::String());
    void askUserToLoadState(const juce::String& fileSuffix = juce::String());

    // Audio playback control
    void startPlaying();
    void stopPlaying();

    // Mute control
    juce::Value& getMuteInputValue();
    bool getProcessorHasPotentialFeedbackLoop() const;

    // Members
    juce::OptionalScopedPointer<juce::PropertySet> settings;
    std::unique_ptr<juce::AudioProcessor> processor;
    juce::Array<PluginInOuts> channelConfiguration;

    bool processorHasPotentialFeedbackLoop = true;
    std::atomic<bool> muteInput{true};
    juce::Value shouldMuteInput;
    juce::AudioBuffer<float> emptyBuffer;
    bool autoOpenMidiDevices;

private:
    void handleCreatePlugin();
    void handleDeletePlugin();
    void init(bool enableAudioInput, const juce::String& preferredDefaultDeviceName);

    juce::File getLastFile() const;
    void setLastFile(const juce::FileChooser& fc);
    static juce::String getFilePatterns(const juce::String& fileSuffix);

    // Value::Listener
    void valueChanged(juce::Value& value) override;

    // Timer
    void timerCallback() override;

    std::unique_ptr<juce::AudioDeviceManager::AudioDeviceSetup> options;
    std::unique_ptr<juce::FileChooser> stateFileChooser;
    juce::ScopedMessageBox messageBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHolder)
};
