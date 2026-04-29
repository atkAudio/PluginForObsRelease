#pragma once

#include "HostAudioProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>

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

    virtual void createPlugin();
    virtual void deletePlugin();

    int getNumInputChannels() const;
    int getNumOutputChannels() const;

    HostAudioProcessorImpl* getHostProcessor() const;

    void savePluginState();
    void reloadPluginState();
    void askUserToSaveState(const juce::String& fileSuffix = juce::String());
    void askUserToLoadState(const juce::String& fileSuffix = juce::String());

    void startPlaying();
    void stopPlaying();

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

    void valueChanged(juce::Value& value) override;

    void timerCallback() override;

    std::unique_ptr<juce::AudioDeviceManager::AudioDeviceSetup> options;
    std::unique_ptr<juce::FileChooser> stateFileChooser;
    juce::ScopedMessageBox messageBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHolder)
};
