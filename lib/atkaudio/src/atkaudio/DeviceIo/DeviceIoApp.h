#pragma once

// #include "../FifoBuffer.h"
#include "../FifoBuffer2.h"
#include "../LookAndFeel.h"
#include "../QtParentedWindow.h"
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

class AudioAppMainWindow final : public atk::QtParentedDocumentWindow
{
public:
    AudioAppMainWindow(DeviceIoApp& demo)
        : atk::QtParentedDocumentWindow(
              "DeviceIo Audio Settings",
              juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
              DocumentWindow::allButtons
          )
        , audioApp(demo)
    {
        setTitleBarButtonsRequired(DocumentWindow::closeButton, false);
        setContentOwned(&demo, false); // Don't take ownership - Impl owns it
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
    DeviceIoApp& audioApp;
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioAppMainWindow)
};