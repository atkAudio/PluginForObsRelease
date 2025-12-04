#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <set>
#include <memory>

namespace atk
{

// Forward declaration - actual value retrieved from OBS at runtime
int getOBSAudioFrameSize();

class ModuleDeviceCoordinator
{
public:
    ModuleDeviceCoordinator() = default;
    ~ModuleDeviceCoordinator() = default;

    // Non-copyable, non-movable
    ModuleDeviceCoordinator(const ModuleDeviceCoordinator&) = delete;
    ModuleDeviceCoordinator& operator=(const ModuleDeviceCoordinator&) = delete;
    ModuleDeviceCoordinator(ModuleDeviceCoordinator&&) = delete;
    ModuleDeviceCoordinator& operator=(ModuleDeviceCoordinator&&) = delete;

    bool tryActivate(juce::AudioIODevice* device)
    {
        const juce::ScopedLock sl(lock);
        if (activeDevice == nullptr || activeDevice == device)
        {
            activeDevice = device;
            return true;
        }
        return false;
    }

    void deactivate(juce::AudioIODevice* device)
    {
        const juce::ScopedLock sl(lock);
        if (activeDevice == device)
            activeDevice = nullptr;
    }

    bool isActive(juce::AudioIODevice* device)
    {
        const juce::ScopedLock sl(lock);
        return activeDevice == device;
    }

private:
    juce::CriticalSection lock;
    juce::AudioIODevice* activeDevice = nullptr;
};

class ModuleOBSAudioDevice : public juce::AudioIODevice
{
private:
    std::shared_ptr<ModuleDeviceCoordinator> coordinator;

public:
    ModuleOBSAudioDevice(
        const juce::String& deviceName,
        std::shared_ptr<ModuleDeviceCoordinator> deviceCoordinator,
        const juce::String& typeName = "Module Audio"
    );

    ~ModuleOBSAudioDevice() override;

    juce::StringArray getOutputChannelNames() override
    {
        juce::StringArray names;
        for (int i = 0; i < obsChannelCount; ++i)
            names.add("Output " + juce::String(i + 1));
        return names;
    }

    juce::StringArray getInputChannelNames() override
    {
        juce::StringArray names;
        for (int i = 0; i < obsChannelCount; ++i)
            names.add("Input " + juce::String(i + 1));
        return names;
    }

    juce::Array<double> getAvailableSampleRates() override
    {
        juce::Array<double> rates;
        rates.add(obsSampleRate);
        return rates;
    }

    juce::Array<int> getAvailableBufferSizes() override
    {
        juce::Array<int> sizes;
        sizes.add(getOBSAudioFrameSize());
        return sizes;
    }

    int getDefaultBufferSize() override
    {
        return getOBSAudioFrameSize();
    }

    juce::String open(
        const juce::BigInteger& inputChannels,
        const juce::BigInteger& outputChannels,
        double sampleRate,
        int bufferSizeSamples
    ) override
    {
        close();

        const juce::ScopedLock sl(lock);

        activeInputChannels.clear();
        activeOutputChannels.clear();

        for (int i = 0; i < obsChannelCount; ++i)
        {
            if (inputChannels[i])
                activeInputChannels.setBit(i);
            if (outputChannels[i])
                activeOutputChannels.setBit(i);
        }

        isOpen_ = true;
        return {};
    }

    void close() override
    {
        if (!isOpen_)
            return;

        stop();

        const juce::ScopedLock sl(lock);
        activeInputChannels.clear();
        activeOutputChannels.clear();
        isOpen_ = false;
    }

    bool isOpen() override
    {
        return isOpen_;
    }

    void start(juce::AudioIODeviceCallback* newCallback) override
    {
        if (!isOpen_ || newCallback == nullptr)
            return;

        stop();

        // Try to become the active device
        if (coordinator && !coordinator->tryActivate(this))
            return;

        const juce::ScopedLock sl(lock);
        callback = newCallback;
        isPlaying_ = true;

        if (callback != nullptr)
            callback->audioDeviceAboutToStart(this);
    }

    void stop() override
    {
        if (!isPlaying_)
            return;

        if (coordinator)
            coordinator->deactivate(this);

        juce::AudioIODeviceCallback* callbackToStop = nullptr;
        {
            const juce::ScopedLock sl(lock);
            callbackToStop = callback;
            callback = nullptr;
            isPlaying_ = false;
        }

        if (callbackToStop != nullptr)
            callbackToStop->audioDeviceStopped();
    }

    bool isPlaying() override
    {
        return isPlaying_;
    }

    juce::String getLastError() override
    {
        return lastError;
    }

    int getCurrentBufferSizeSamples() override
    {
        return getOBSAudioFrameSize();
    }

    double getCurrentSampleRate() override
    {
        return obsSampleRate;
    }

    int getCurrentBitDepth() override
    {
        return 32; // Float32
    }

    juce::BigInteger getActiveOutputChannels() const override
    {
        return activeOutputChannels;
    }

    juce::BigInteger getActiveInputChannels() const override
    {
        return activeInputChannels;
    }

    int getOutputLatencyInSamples() override
    {
        return 0;
    }

    int getInputLatencyInSamples() override
    {
        return 0;
    }

    virtual void processExternalAudio(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        double sampleRate = 0.0
    )
    {
        // Check if we're the active device
        if (!coordinator || !coordinator->isActive(this))
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            return;
        }

        const juce::ScopedLock sl(lock);

        if (!isOpen_ || callback == nullptr || !isPlaying_)
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            return;
        }

        // Build arrays with only the active channels
        const int numActiveInputs = activeInputChannels.countNumberOfSetBits();
        const int numActiveOutputs = activeOutputChannels.countNumberOfSetBits();

        if (numActiveInputs == 0 && numActiveOutputs == 0)
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            return;
        }

        // Prepare channel arrays
        std::vector<const float*> activeInputPtrs(numActiveInputs);
        std::vector<float*> activeOutputPtrs(numActiveOutputs);

        juce::AudioBuffer<float> tempOutputBuffer;
        if (numActiveOutputs != numOutputChannels)
            tempOutputBuffer.setSize(numActiveOutputs, numSamples, false, false, true);

        // Map active input channels
        int activeIdx = 0;
        for (int ch = 0; ch < numInputChannels && activeIdx < numActiveInputs; ++ch)
            if (activeInputChannels[ch])
                activeInputPtrs[activeIdx++] = inputChannelData[ch];

        // Map active output channels
        if (numActiveOutputs != numOutputChannels)
        {
            for (int i = 0; i < numActiveOutputs; ++i)
                activeOutputPtrs[i] = tempOutputBuffer.getWritePointer(i);
        }
        else
        {
            activeIdx = 0;
            for (int ch = 0; ch < numOutputChannels && activeIdx < numActiveOutputs; ++ch)
                if (activeOutputChannels[ch])
                    activeOutputPtrs[activeIdx++] = outputChannelData[ch];
        }

        // Create context
        juce::AudioIODeviceCallbackContext context;
        uint64_t hostTimeNs =
            juce::Time::getHighResolutionTicks() * 1000000000ull / juce::Time::getHighResolutionTicksPerSecond();
        context.hostTimeNs = &hostTimeNs;

        // Call JUCE callback
        callback->audioDeviceIOCallbackWithContext(
            activeInputPtrs.data(),
            numActiveInputs,
            activeOutputPtrs.data(),
            numActiveOutputs,
            numSamples,
            context
        );

        // Copy temp buffer back if needed
        if (numActiveOutputs != numOutputChannels)
        {
            activeIdx = 0;
            for (int ch = 0; ch < numOutputChannels && activeIdx < numActiveOutputs; ++ch)
            {
                if (activeOutputChannels[ch])
                {
                    const float* src = tempOutputBuffer.getReadPointer(activeIdx);
                    std::copy(src, src + numSamples, outputChannelData[ch]);
                    activeIdx++;
                }
                else
                {
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
                }
            }
        }
        else
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (!activeOutputChannels[ch] && outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
        }
    }

protected:
    juce::CriticalSection lock;
    juce::AudioIODeviceCallback* callback = nullptr;

    int obsChannelCount = 2;
    double obsSampleRate = 48000.0;

    juce::BigInteger activeInputChannels;
    juce::BigInteger activeOutputChannels;

    bool isOpen_ = false;
    bool isPlaying_ = false;
    juce::String lastError;
};

} // namespace atk
