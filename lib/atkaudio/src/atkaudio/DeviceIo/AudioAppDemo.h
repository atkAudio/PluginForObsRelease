#pragma once

#include "../FifoBuffer.h"
#include "../LookAndFeel.h"
#include "SettingsComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

using namespace juce;

//==============================================================================
class AudioAppDemo final : public AudioAppComponent
{
public:
    //==============================================================================
    AudioAppDemo(AudioDeviceManager& deviceManager, int numInputChannels, int numOutputChannels, double obsSampleRate)
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

    ~AudioAppDemo() override
    {
        shutdownAudio();
    }

    //==============================================================================
    void prepareToPlay(int samplesPerBlockExpected, double newSampleRate) override
    {
        juce::ScopedLock lock(prepareLock);

        sampleRate = newSampleRate;
        bufferSize = samplesPerBlockExpected;

        auto inputChannels = deviceManager.getCurrentAudioDevice()->getActiveInputChannels().countNumberOfSetBits();
        auto outputChannels = deviceManager.getCurrentAudioDevice()->getActiveOutputChannels().countNumberOfSetBits();

        inputFifo.setSize(inputChannels, (int)sampleRate);
        outputFifo.setSize(outputChannels, (int)sampleRate);

        interpolators.resize(outputChannels);
        for (auto& i : interpolators)
            i.reset();

        isPrepared = true;
    }

    void getNextAudioBlock(const AudioSourceChannelInfo& bufferToFill) override
    {
        auto inputChannels = inputFifo.getNumChannels();
        auto outputChannels = outputFifo.getNumChannels();

        auto numSamples = bufferToFill.numSamples;
        auto& buffer = bufferToFill.buffer;

        for (auto i = 0; i < inputChannels; ++i)
            inputFifo.write(buffer->getWritePointer(i), i, numSamples, i == inputChannels - 1);

        auto sampleRatio = getRemoteSampleRate() / sampleRate;
        auto remoteBufSize = getRemoteBufferSize();

        if (remoteBufSize == 0 || sampleRatio == 0)
            return;

        auto sampleRatioCorrection = 1.0;
        if (speedUp)
            sampleRatioCorrection *= 1.00111;
        if (speedDown)
            sampleRatioCorrection *= (1 / 1.00111);

        sampleRatio *= sampleRatioCorrection;

        auto numOutputSamplesReady = outputFifo.getNumReady();
        if (numOutputSamplesReady / sampleRatio < numSamples)
            return;

        tempBuffer.resize(numOutputSamplesReady, 0.0f);

        int consumedSamples = 0;

        for (auto i = 0; i < outputChannels; ++i)
        {
            outputFifo.read(tempBuffer.data(), i, numOutputSamplesReady, false);
            consumedSamples = interpolators[i].process(
                sampleRatio,
                tempBuffer.data(),
                buffer->getWritePointer(i),
                numSamples,
                numOutputSamplesReady,
                0
            );
        }

        if (consumedSamples > outputFifo.getNumReady())
            consumedSamples = outputFifo.getNumReady();

        outputFifo.advanceRead(consumedSamples);

        sampleRatio = getRemoteSampleRate() / sampleRate;

        auto minSamples = std::min(bufferSize, (int)(remoteBufSize / sampleRatio));
        auto maxSamples = std::max(bufferSize, (int)(remoteBufSize / sampleRatio));

        maxSamples = maxSamples * 2;
        if (speedUp)
            maxSamples /= 2;

        if (speedDown)
            minSamples *= 2;

        numOutputSamplesReady = (int)(outputFifo.getNumReady() / sampleRatio);

        if (numOutputSamplesReady < minSamples)
            speedDown = true;
        else if (speedDown)
        {
#if JUCE_DEBUG
            auto timeAndDate = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S");
            DBG("output speed down " << timeAndDate);
#endif
            speedDown = false;
        }

        if (numOutputSamplesReady > maxSamples)
            speedUp = true;
        else if (speedUp)
        {
#if JUCE_DEBUG
            auto timeAndDate = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S");
            DBG("output speed up " << timeAndDate);
#endif
            speedUp = false;
        }
    }

    void releaseResources() override
    {
        juce::ScopedLock lock(prepareLock);
        isPrepared = false;
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

    auto getPrepareLock() -> juce::CriticalSection&
    {
        return prepareLock;
    }

    auto getIsPrepared() -> bool
    {
        return isPrepared;
    }

    auto getSampleRate() -> double
    {
        return sampleRate;
    }

    auto getBufferSize() -> int
    {
        return bufferSize;
    }

    auto getInputFifo() -> atk::FifoBuffer&
    {
        return inputFifo;
    }

    auto getOutputFifo() -> atk::FifoBuffer&
    {
        return outputFifo;
    }

    void setRemoteSampleRate(double newSampleRate)
    {
        remoteSampleRate.store(newSampleRate, std::memory_order_release);
    }

    double getRemoteSampleRate() const
    {
        return remoteSampleRate.load(std::memory_order_acquire);
    }

    void setRemoteBufferSize(int newBufferSize)
    {
        remoteBufferSize.store(newBufferSize, std::memory_order_release);
    }

    int getRemoteBufferSize() const
    {
        return remoteBufferSize.load(std::memory_order_acquire);
    }

private:
    juce::CriticalSection prepareLock;
    bool isPrepared;

    double sampleRate = 0.0;
    int bufferSize = 0;

    std::atomic<double> remoteSampleRate;
    std::atomic_int remoteBufferSize;

    atk::FifoBuffer inputFifo;
    atk::FifoBuffer outputFifo;

    std::vector<float> tempBuffer;
    std::vector<juce::Interpolators::Lagrange> interpolators;

    bool speedUp = false;
    bool speedDown = false;

    juce::AudioDeviceManager& deviceManager;
    juce::AudioDeviceManager::AudioDeviceSetup audioSetup;

    SettingsComponent settingsComponent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioAppDemo)
};

class AudioAppMainWindow final : public DocumentWindow
{
public:
    AudioAppMainWindow(AudioAppDemo& demo)
        : DocumentWindow("", Colours::lightgrey, DocumentWindow::minimiseButton | DocumentWindow::closeButton)
        , audioApp(demo)
    {
        setContentOwned(&demo, true);
        setResizable(true, false);
        centreWithSize(demo.getWidth(), demo.getHeight());
        setVisible(false);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    AudioAppDemo& audioApp;
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioAppMainWindow)
};