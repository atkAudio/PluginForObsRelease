#pragma once

#include "../../DeviceIo/DeviceIoApp.h"
#include "../../DeviceIo/SettingsComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

class Ph2DeviceIoProcessor
    : public juce::AudioProcessor
    , public juce::AudioIODeviceCallback
{
public:
    Ph2DeviceIoProcessor()
    {
        deviceManager.addAudioCallback(this);
    }

    ~Ph2DeviceIoProcessor() override
    {
    }

    const juce::String getName() const override
    {
        return "Device Io";
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        auto numChannels = getMainBusNumInputChannels();
        toHostBuffer.prepareReader(sampleRate, numChannels, samplesPerBlock);
        fromHostBuffer.prepareWriter(sampleRate, numChannels, samplesPerBlock);
    }

    void releaseResources() override
    {
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        auto numSamples = buffer.getNumSamples();

        auto numInputChannels = getMainBusNumInputChannels();
        auto numOutputChannels = getMainBusNumOutputChannels();

        auto* inputData = buffer.getArrayOfReadPointers();
        fromHostBuffer.write(inputData, numInputChannels, numSamples);

        auto* outputData = buffer.getArrayOfWritePointers();
        toHostBuffer.read(outputData, numOutputChannels, numSamples);
    }

    juce::AudioProcessorEditor* createEditor() override;

    bool hasEditor() const override
    {
        return true;
    }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return true;
    }

    double getTailLengthSeconds() const override
    {
        return 0.0;
    }

    int getNumPrograms() override
    {
        return 1;
    }

    int getCurrentProgram() override
    {
        return 0;
    }

    void setCurrentProgram(int) override
    {
    }

    const juce::String getProgramName(int) override
    {
        return {};
    }

    void changeProgramName(int, const juce::String&) override
    {
    }

    bool acceptsMidi() const override
    {
        return false;
    }

    bool producesMidi() const override
    {
        return false;
    }

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        auto xml = deviceManager.createStateXml();
        if (xml)
            copyXmlToBinary(*xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        juce::MemoryBlock block(data, sizeInBytes);
        auto xml = getXmlFromBinary(block.getData(), block.getSize());

        auto numChannels = getMainBusNumInputChannels();
        if (xml != nullptr)
            deviceManager.initialise(numChannels, numChannels, xml.get(), true);
    }

    auto& getDeviceManager()
    {
        return deviceManager;
    }

    auto& getToHostBuffer()
    {
        return toHostBuffer;
    }

    auto& getFromHostBuffer()
    {
        return fromHostBuffer;
    }

private:
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const AudioIODeviceCallbackContext& context
    ) override
    {
        if (numInputChannels > 0)
            toHostBuffer.write(inputChannelData, numInputChannels, numSamples);

        if (numOutputChannels > 0)
            fromHostBuffer.read(outputChannelData, numOutputChannels, numSamples);
    }

    void audioDeviceAboutToStart(AudioIODevice* device) override
    {
        fromHostBuffer.prepareReader(
            device->getCurrentSampleRate(),
            device->getActiveOutputChannels().countNumberOfSetBits(),
            device->getCurrentBufferSizeSamples()
        );
        toHostBuffer.prepareWriter(
            device->getCurrentSampleRate(),
            device->getActiveInputChannels().countNumberOfSetBits(),
            device->getCurrentBufferSizeSamples()
        );
    }

    void audioDeviceStopped() override
    {
        return;
    }

    juce::AudioDeviceManager deviceManager;
    SyncBuffer toHostBuffer;
    SyncBuffer fromHostBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Ph2DeviceIoProcessor)
};

;

class SimpleAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    SimpleAudioProcessorEditor(Ph2DeviceIoProcessor& p)
        : juce::AudioProcessorEditor(&p)
        , processor(p)
        , settingsComponent(p.getDeviceManager(), 256, 256)
    {
        addAndMakeVisible(settingsComponent);
        settingsComponent.setSize(500, 550);
        settingsComponent.setToRecommendedSize();
        setSize(settingsComponent.getWidth(), settingsComponent.getHeight());
    }

    void paint(juce::Graphics& g) override
    {
    }

private:
    Ph2DeviceIoProcessor& processor;
    SettingsComponent settingsComponent;
};

inline juce::AudioProcessorEditor* Ph2DeviceIoProcessor::createEditor()
{
    return new SimpleAudioProcessorEditor(*this);
}