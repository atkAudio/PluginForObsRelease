#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <obs-module.h>

#define IO_TYPE "OBS"
#define IO_NAME "atkAudio"

// #define IO_MAX_CHANNELS 8

class VirtualAudioIODevice : public juce::AudioIODevice
{
public:
    VirtualAudioIODevice(const juce::String& name = IO_NAME, const juce::String& type = IO_TYPE)
        : juce::AudioIODevice(name, type)
        , numChannels(audio_output_get_channels(obs_get_audio()))
        , sampleRate(audio_output_get_sample_rate(obs_get_audio()))
    {
    }

    ~VirtualAudioIODevice() override
    {
        close();
    }

    // Standard JUCE overrides for a minimal device
    juce::StringArray getOutputChannelNames() override
    {
        juce::StringArray outputChannelNames;
        for (int i = 0; i < numChannels; ++i)
            outputChannelNames.add(juce::String(i + 1));
        return outputChannelNames;
    }

    juce::StringArray getInputChannelNames() override
    {
        return getOutputChannelNames();
    }

    juce::Array<double> getAvailableSampleRates() override
    {
        juce::Array<double> rates;
        rates.add(sampleRate);

        return rates;
    }

    juce::Array<int> getAvailableBufferSizes() override
    {
        return {getDefaultBufferSize()};
    }

    int getDefaultBufferSize() override
    {
        return bufferSize;
    }

    double getCurrentSampleRate() override
    {
        return sampleRate;
    }

    int getCurrentBufferSizeSamples() override
    {
        return bufferSize;
    }

    int getCurrentBitDepth() override
    {
        return 32;
    }

    juce::BigInteger getActiveOutputChannels() const override
    {
        juce::BigInteger b;
        b.setRange(0, numChannels, true);
        return b;
    }

    juce::BigInteger getActiveInputChannels() const override
    {
        juce::BigInteger b;
        b.setRange(0, numChannels * 2, true);
        return b;
    }

    int getOutputLatencyInSamples() override
    {
        return 0;
    }

    int getInputLatencyInSamples() override
    {
        return 0;
    }

    int getXRunCount() const noexcept override
    {
        return 0;
    }

    bool isOpen() override
    {
        return opened;
    }

    bool isPlaying() override
    {
        return playing;
    }

    juce::String getLastError() override
    {
        return lastError;
    }

    juce::String
    open(const juce::BigInteger&, const juce::BigInteger&, double newSampleRate, int newBufferSize) override
    {
        close();
        sampleRate = newSampleRate;
        bufferSize = newBufferSize;
        inputBuffer.setSize(numChannels * 2, bufferSize, false, false, true);
        outputBuffer.setSize(numChannels, bufferSize, false, false, true);
        opened = true;
        return {};
    }

    void close() override
    {
        opened = false;
        stop();
    }

    void start(juce::AudioIODeviceCallback* cb) override
    {
        juce::ScopedLock lock(callbackLock);
        if (cb == nullptr)
            return;

        cb->audioDeviceAboutToStart(this);
        currentCallback = cb;
        playing = true;
    }

    void stop() override
    {
        juce::ScopedLock lock(callbackLock);
        playing = false;
        currentCallback = nullptr;
    }

    // --- core API: your code calls this to feed audio to the virtual device
    // inputData: channels x samples; outputBuffer is filled by the callback
    juce::AudioIODeviceCallbackContext context;

    void process(const float* const* inputData, int numInputChannels, int numSamples)
    {
        juce::ScopedLock lock(callbackLock);

        if (playing && currentCallback)
        {
            currentCallback->audioDeviceIOCallbackWithContext(
                inputData,
                numInputChannels,
                outputBuffer.getArrayOfWritePointers(),
                // outputBuffer.getNumChannels(),
                numInputChannels,
                numSamples,
                context
            );
            for (int ch = 0; ch < numInputChannels && ch < outputBuffer.getNumChannels(); ch++)
                std::memcpy(
                    const_cast<float*>(inputData[ch]),
                    outputBuffer.getReadPointer(ch),
                    sizeof(float) * numSamples
                );
        }
    }

    // Optional: get output from last callback
    const juce::AudioBuffer<float>& getLastOutput() const
    {
        return outputBuffer;
    }

private:
    juce::CriticalSection callbackLock;

    bool opened = false, playing = false;
    juce::AudioIODeviceCallback* currentCallback = nullptr;
    juce::String lastError;
    double sampleRate;
    int bufferSize = AUDIO_OUTPUT_FRAMES; // Default buffer size
    int numChannels;
    juce::AudioBuffer<float> inputBuffer, outputBuffer;
};

//
struct VirtualAudioIODeviceType : public juce::AudioIODeviceType
{
    VirtualAudioIODeviceType()
        : juce::AudioIODeviceType(IO_TYPE)
    {
        scanForDevices();
    }

    void scanForDevices() override
    {
        names = {IO_NAME};
    }

    juce::StringArray getDeviceNames(bool) const override
    {
        return names;
    }

    int getDefaultDeviceIndex(bool) const override
    {
        return 0;
    }

    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override
    {
        (void)device;
        return getDefaultDeviceIndex(asInput);
    }

    bool hasSeparateInputsAndOutputs() const override
    {
        return false;
    }

    juce::AudioIODevice* createDevice(const juce::String&, const juce::String&) override
    {
        return new VirtualAudioIODevice();
    }

    juce::StringArray names;
};
