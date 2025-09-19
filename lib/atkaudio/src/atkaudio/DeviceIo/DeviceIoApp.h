#pragma once

// #include "../FifoBuffer.h"
#include "../FifoBuffer2.h"
#include "../LookAndFeel.h"
#include "SettingsComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

using namespace juce;

//==============================================================================
class DeviceIoApp final : public AudioAppComponent
{
public:
    //==============================================================================
    DeviceIoApp(AudioDeviceManager& deviceManager, int numInputChannels, int numOutputChannels, double obsSampleRate)
        : AudioAppComponent(deviceManager)
        , deviceManager(deviceManager)
        , settingsComponent(deviceManager, numInputChannels, numOutputChannels)
    {
        setAudioChannels(numInputChannels, numOutputChannels);
        settingsComponent.setSize(500, 550);
        settingsComponent.setToRecommendedSize();
        addAndMakeVisible(settingsComponent);

        deviceManager.initialise(0, 0, nullptr, false);

        setSize(settingsComponent.getWidth(), settingsComponent.getHeight());

        (void)obsSampleRate; // OBS sample rate is not used in this demo, but can be set if needed
    }

    ~DeviceIoApp() override
    {
        shutdownAudio();
    }

    //==============================================================================
    void prepareToPlay(int samplesPerBlockExpected, double newSampleRate) override
    {
        inputChannels = deviceManager.getCurrentAudioDevice()->getActiveInputChannels().countNumberOfSetBits();
        outputChannels = deviceManager.getCurrentAudioDevice()->getActiveOutputChannels().countNumberOfSetBits();
        sampleRate = newSampleRate;

        toObsBuffer.clearPrepared();
        fromObsBuffer.clearPrepared();
    }

    // processBlock
    void getNextAudioBlock(const AudioSourceChannelInfo& bufferToFill) override
    {
        if (inputChannels > 0)
            toObsBuffer.write(
                bufferToFill.buffer->getArrayOfReadPointers(),
                inputChannels,
                bufferToFill.numSamples,
                sampleRate
            );

        if (outputChannels > 0)
            fromObsBuffer.read(
                bufferToFill.buffer->getArrayOfWritePointers(),
                outputChannels,
                bufferToFill.numSamples,
                sampleRate
            );
    }

    void releaseResources() override
    {
    }

    //==============================================================================
    void paint(Graphics& g) override
    {
        (void)g;
    }

    void resized() override
    {
        // This is called when the component is resized.
        // If you add any child components, this is where you should
        // update their positions.
    }

    auto& getFromObsBuffer()
    {
        return fromObsBuffer;
    }

    auto& getToObsBuffer()
    {
        return toObsBuffer;
    }

private:
    juce::AudioDeviceManager& deviceManager;
    juce::AudioDeviceManager::AudioDeviceSetup audioSetup;
    int inputChannels = 0;
    int outputChannels = 0;
    double sampleRate = 0.0;

    SyncBuffer toObsBuffer;
    SyncBuffer fromObsBuffer;

    SettingsComponent settingsComponent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceIoApp)
};

class AudioAppMainWindow final : public DocumentWindow
{
public:
    AudioAppMainWindow(DeviceIoApp& demo)
        : DocumentWindow("", Colours::lightgrey, DocumentWindow::minimiseButton | DocumentWindow::closeButton, false)
        , audioApp(demo)
    {
        setContentOwned(&demo, true);
        setResizable(true, false);

        // Position title bar buttons on the right (Windows-style), like Plugin Host
        setTitleBarButtonsRequired(DocumentWindow::minimiseButton | DocumentWindow::closeButton, false);

        centreWithSize(demo.getWidth(), demo.getHeight());
        setVisible(false);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    DeviceIoApp& audioApp;
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioAppMainWindow)
};