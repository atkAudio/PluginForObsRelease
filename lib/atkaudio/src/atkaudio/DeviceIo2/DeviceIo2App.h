#pragma once

#include "../FifoBuffer2.h"
#include "../LookAndFeel.h"
#include "../QtParentedWindow.h"
#include "DeviceIo2SettingsComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

using namespace juce;

//==============================================================================
class DeviceIo2App final : public AudioAppComponent
{
public:
    //==============================================================================
    DeviceIo2App(AudioDeviceManager& deviceManager, int numInputChannels, int numOutputChannels, double obsSampleRate)
        : AudioAppComponent(deviceManager)
        , deviceManager(deviceManager)
        , settingsComponent(deviceManager, numInputChannels, numOutputChannels)
    {
        setAudioChannels(numInputChannels, numOutputChannels);
        settingsComponent.setSize(700, 600); // Larger size to accommodate routing matrix UI
        settingsComponent.setToRecommendedSize();
        addAndMakeVisible(settingsComponent);

        deviceManager.initialise(0, 0, nullptr, false);

        setSize(settingsComponent.getWidth(), settingsComponent.getHeight());

        (void)obsSampleRate; // OBS sample rate is not used in this demo, but can be set if needed
    }

    ~DeviceIo2App() override
    {
        shutdownAudio();
    }

    //==============================================================================
    void prepareToPlay(int samplesPerBlockExpected, double newSampleRate) override
    {
        inputChannels = deviceManager.getCurrentAudioDevice()->getActiveInputChannels().countNumberOfSetBits();
        outputChannels = deviceManager.getCurrentAudioDevice()->getActiveOutputChannels().countNumberOfSetBits();
        sampleRate = newSampleRate;

        deviceInputBuffer.clearPrepared();
        deviceOutputBuffer.clearPrepared();
    }

    // processBlock - called by audio device
    void getNextAudioBlock(const AudioSourceChannelInfo& bufferToFill) override
    {
        // Read from device input into deviceInputBuffer
        if (inputChannels > 0)
            deviceInputBuffer.write(
                bufferToFill.buffer->getArrayOfReadPointers(),
                inputChannels,
                bufferToFill.numSamples,
                sampleRate
            );

        // Write to device output from deviceOutputBuffer
        if (outputChannels > 0)
            deviceOutputBuffer.read(
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

    auto& getDeviceInputBuffer()
    {
        return deviceInputBuffer;
    }

    auto& getDeviceOutputBuffer()
    {
        return deviceOutputBuffer;
    }

private:
    juce::AudioDeviceManager& deviceManager;
    juce::AudioDeviceManager::AudioDeviceSetup audioSetup;
    int inputChannels = 0;
    int outputChannels = 0;
    double sampleRate = 0.0;

    SyncBuffer deviceInputBuffer;
    SyncBuffer deviceOutputBuffer;

    DeviceIo2SettingsComponent settingsComponent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceIo2App)
};

class AudioAppMainWindow final : public atk::QtParentedDocumentWindow
{
public:
    AudioAppMainWindow(DeviceIo2App& demo)
        : atk::QtParentedDocumentWindow("", Colours::lightgrey, DocumentWindow::allButtons)
        , audioApp(demo)
    {
        setTitleBarButtonsRequired(DocumentWindow::minimiseButton | DocumentWindow::closeButton, false);
        setContentOwned(&demo, true);
        setResizable(true, false);

        centreWithSize(demo.getWidth(), demo.getHeight());
    }

    ~AudioAppMainWindow() override
    {
        // Note: setVisible(false) is handled by atk::destroy() force-hide before this destructor runs
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    DeviceIo2App& audioApp;
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioAppMainWindow)
};
