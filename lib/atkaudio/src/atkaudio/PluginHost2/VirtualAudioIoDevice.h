#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <obs-module.h>

#define IO_TYPE "OBS"
#define IO_NAME "atkAudio"

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
        b.setRange(0, numChannels, true);
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
        if (cb == nullptr)
            return;

        cb->audioDeviceAboutToStart(this);
        currentCallback = cb;
        playing = true;
    }

    void stop() override
    {
        if (currentCallback)
            currentCallback->audioDeviceStopped();
        playing = false;
        currentCallback = nullptr;
    }

    auto* getAudioDeviceCallback() const noexcept
    {
        return currentCallback;
    }

private:
    bool opened = false, playing = false;
    juce::AudioIODeviceCallback* currentCallback = nullptr;
    juce::String lastError;
    double sampleRate;
    int bufferSize = AUDIO_OUTPUT_FRAMES; // Default buffer size
    int numChannels;
};

//
struct VirtualAudioIODeviceType : public juce::AudioIODeviceType
{
    VirtualAudioIODeviceType()
        : juce::AudioIODeviceType(IO_TYPE)
    {
        scanForDevices();
    }

    bool isDeviceTypeActive() const
    {
        return true; // Always active for virtual devices
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
