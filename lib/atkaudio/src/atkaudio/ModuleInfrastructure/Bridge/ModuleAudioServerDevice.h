#pragma once

#include "ModuleAudioDevice.h"
#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <thread>

namespace atk
{

struct AudioServerDeviceInfo
{
    juce::String deviceName;
    juce::String deviceType; // "ASIO", "Windows Audio", "CoreAudio", "ALSA"

    juce::String getDisplayName() const
    {
        // Don't add type suffix if device name already contains the type
        if (deviceName.containsIgnoreCase(deviceType))
            return deviceName;

        return deviceName + " (" + deviceType + ")";
    }
};

class ModuleAudioServerDevice
    : public juce::AudioIODevice
    , public juce::AudioIODeviceCallback
{
private:
    std::shared_ptr<ModuleDeviceCoordinator> coordinator;

public:
    ModuleAudioServerDevice(
        const juce::String& displayName,
        const juce::String& actualDeviceName,
        const juce::String& deviceType,
        std::shared_ptr<ModuleDeviceCoordinator> deviceCoordinator,
        const juce::String& typeName = "Module Audio"
    )
        : juce::AudioIODevice(displayName, typeName)
        , coordinator(deviceCoordinator)
        , actualDeviceName(actualDeviceName)
        , deviceType(deviceType)
    {
    }

    ~ModuleAudioServerDevice() override
    {
        isDestroying.store(true, std::memory_order_release);

        if (auto* server = AudioServer::getInstanceWithoutCreating())
            server->unregisterDirectCallback(actualDeviceName.toStdString(), this);

        // Wait for any active callbacks to exit
        while (activeCallbackCount.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();

        close();
    }

    juce::StringArray getOutputChannelNames() override
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getDeviceChannelNames(actualDeviceName, false);
        return {};
    }

    juce::StringArray getInputChannelNames() override
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getDeviceChannelNames(actualDeviceName, true);
        return {};
    }

    juce::Array<double> getAvailableSampleRates() override
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getAvailableSampleRates(actualDeviceName);
        return {44100.0, 48000.0};
    }

    juce::Array<int> getAvailableBufferSizes() override
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getAvailableBufferSizes(actualDeviceName);
        return {512};
    }

    int getDefaultBufferSize() override
    {
        auto sizes = getAvailableBufferSizes();
        return sizes.isEmpty() ? 512 : sizes[0];
    }

    juce::String open(
        const juce::BigInteger& inputChannels,
        const juce::BigInteger& outputChannels,
        double sampleRate,
        int bufferSizeSamples
    ) override
    {
        bool needsReopen = false;

        if (isOpen_)
        {
            const juce::ScopedLock sl(lock);
            if ((sampleRate > 0.0 && !juce::exactlyEqual(currentSampleRate, sampleRate))
                || (bufferSizeSamples > 0 && currentBufferSize != bufferSizeSamples))
            {
                needsReopen = true;
            }
        }

        if (!isOpen_ || !needsReopen)
        {
            const juce::ScopedLock sl(lock);

            int actualInputChannelCount = 0;
            int actualOutputChannelCount = 0;
            if (auto* server = AudioServer::getInstanceWithoutCreating())
            {
                actualInputChannelCount = server->getDeviceNumChannels(actualDeviceName, true);
                actualOutputChannelCount = server->getDeviceNumChannels(actualDeviceName, false);
            }

            activeInputChannels.clear();
            activeOutputChannels.clear();

            for (int i = 0; i < actualInputChannelCount; ++i)
                if (inputChannels[i])
                    activeInputChannels.setBit(i);

            for (int i = 0; i < actualOutputChannelCount; ++i)
                if (outputChannels[i])
                    activeOutputChannels.setBit(i);
        }

        if (needsReopen || !isOpen_)
        {
            juce::AudioDeviceManager::AudioDeviceSetup currentSetup;
            bool hasCurrentSetup = false;
            if (needsReopen)
            {
                if (auto* server = AudioServer::getInstanceWithoutCreating())
                    hasCurrentSetup = server->getCurrentDeviceSetup(actualDeviceName, currentSetup);
                close();
            }

            if (!isOpen_)
            {
                if (auto* server = AudioServer::getInstance())
                {
                    juce::AudioDeviceManager::AudioDeviceSetup setup;

                    if (needsReopen && hasCurrentSetup)
                    {
                        setup.sampleRate = (sampleRate > 0.0 && !juce::exactlyEqual(currentSampleRate, sampleRate))
                                             ? sampleRate
                                             : currentSetup.sampleRate;
                        setup.bufferSize = (bufferSizeSamples > 0 && currentBufferSize != bufferSizeSamples)
                                             ? bufferSizeSamples
                                             : currentSetup.bufferSize;
                    }
                    else
                    {
                        setup.sampleRate = 0.0;
                        setup.bufferSize = 0;
                    }

                    // Don't pass channel config - AudioServer opens with all channels,
                    // and ModuleAudioServerDevice filters to active ones in the callback

                    if (!server->registerDirectCallback(actualDeviceName.toStdString(), this, setup))
                        return "Failed to register with AudioServer";

                    const juce::ScopedLock sl(lock);
                    currentSampleRate = server->getCurrentSampleRate(actualDeviceName);
                    currentBufferSize = server->getCurrentBufferSize(actualDeviceName);
                }

                isOpen_ = true;
            }
        }

        return {};
    }

    void close() override
    {
        if (!isOpen_)
            return;

        stop();

        if (auto* server = AudioServer::getInstanceWithoutCreating())
            server->unregisterDirectCallback(actualDeviceName.toStdString(), this);

        {
            const juce::ScopedLock sl(lock);
            isOpen_ = false;
        }
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

        if (coordinator && !coordinator->tryActivate(this))
            return;

        {
            const juce::ScopedLock sl(lock);
            userCallback = newCallback;
            isPlaying_ = true;
        }

        if (userCallback != nullptr)
            userCallback->audioDeviceAboutToStart(this);
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
            callbackToStop = userCallback;
            userCallback = nullptr;
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
        return {};
    }

    int getCurrentBufferSizeSamples() override
    {
        if (currentBufferSize > 0)
            return currentBufferSize;
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getCurrentBufferSize(actualDeviceName);
        return 0;
    }

    double getCurrentSampleRate() override
    {
        if (currentSampleRate > 0.0)
            return currentSampleRate;
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getCurrentSampleRate(actualDeviceName);
        return 0.0;
    }

    int getCurrentBitDepth() override
    {
        return 16;
    }

    juce::BigInteger getActiveInputChannels() const override
    {
        return activeInputChannels;
    }

    juce::BigInteger getActiveOutputChannels() const override
    {
        return activeOutputChannels;
    }

    int getOutputLatencyInSamples() override
    {
        return 0;
    }

    int getInputLatencyInSamples() override
    {
        return 0;
    }

private:
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context
    ) override
    {
        // Early exit if destroying to prevent use-after-free
        if (isDestroying.load(std::memory_order_acquire))
            return;

        // Track active callbacks with RAII guard
        activeCallbackCount.fetch_add(1, std::memory_order_acquire);

        struct Guard
        {
            std::atomic<int>& count;

            ~Guard()
            {
                count.fetch_sub(1, std::memory_order_release);
            }
        } guard{activeCallbackCount};

        // Double-check after incrementing
        if (isDestroying.load(std::memory_order_acquire))
            return;

        // Check if we're the active device
        if (!coordinator || !coordinator->isActive(this))
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            return;
        }

        const juce::ScopedLock sl(lock);

        if (!isOpen_ || userCallback == nullptr || !isPlaying_)
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            return;
        }

        // AudioServer always passes ALL hardware channels
        // We need to filter to only the active channels before passing to JUCE's CallbackMaxSizeEnforcer
        // because it sizes its arrays based on getActiveInputChannels().countNumberOfSetBits()

        int numActiveInputs = activeInputChannels.countNumberOfSetBits();
        int numActiveOutputs = activeOutputChannels.countNumberOfSetBits();

        // Resize temp output buffer if needed (for channel filtering)
        // We receive ALL channels from AudioServer but need to pass only active channels to user callback
        if (tempOutputBuffer.getNumChannels() < numActiveOutputs || tempOutputBuffer.getNumSamples() < numSamples)
            tempOutputBuffer.setSize(numActiveOutputs, numSamples, false, false, true);

        // Safety check: ensure temp buffer is valid after resize
        if (tempOutputBuffer.getNumChannels() < numActiveOutputs || tempOutputBuffer.getNumSamples() < numSamples)
        {
            // Failed to allocate - clear outputs and return
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            return;
        }

        // Build filtered input channel pointers (inputs are read-only, so we can use direct pointers)
        activeInputPtrs.clear();
        int activeIdx = 0;
        for (int ch = 0; ch < numInputChannels && activeIdx < numActiveInputs; ++ch)
        {
            if (activeInputChannels[ch] && inputChannelData[ch] != nullptr)
            {
                activeInputPtrs.add(inputChannelData[ch]);
                activeIdx++;
            }
        }

        // Build output pointers from temp buffer
        // Ensure vector is large enough for the number of active outputs
        if (static_cast<int>(activeOutputPtrs.size()) < numActiveOutputs)
            activeOutputPtrs.resize(numActiveOutputs);

        for (int i = 0; i < numActiveOutputs; ++i)
        {
            auto* ptr = tempOutputBuffer.getWritePointer(i);
            if (ptr == nullptr)
            {
                // Invalid buffer pointer - clear outputs and return
                for (int ch = 0; ch < numOutputChannels; ++ch)
                    if (outputChannelData[ch] != nullptr)
                        juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
                return;
            }
            activeOutputPtrs[i] = ptr;
        }

        // Call user callback with filtered channels
        userCallback->audioDeviceIOCallbackWithContext(
            activeInputPtrs.getRawDataPointer(),
            numActiveInputs,
            activeOutputPtrs.data(),
            numActiveOutputs,
            numSamples,
            context
        );

        // Copy filtered output back to hardware channels
        activeIdx = 0;
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (activeOutputChannels[ch] && outputChannelData[ch] != nullptr && activeIdx < numActiveOutputs)
            {
                // Safety check: ensure we have valid buffer pointers
                const float* srcPtr = tempOutputBuffer.getReadPointer(activeIdx);
                if (srcPtr != nullptr && activeIdx < tempOutputBuffer.getNumChannels())
                {
                    std::copy(srcPtr, srcPtr + numSamples, outputChannelData[ch]);
                }
                else
                {
                    // Invalid source pointer - clear this channel
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
                }
                activeIdx++;
            }
            else if (outputChannelData[ch] != nullptr)
            {
                // Clear non-active output channels
                juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            }
        }
    }

    void audioDeviceAboutToStart(juce::AudioIODevice*) override
    {
    }

    void audioDeviceStopped() override
    {
    }

    juce::String actualDeviceName;
    juce::String deviceType;
    juce::AudioIODeviceCallback* userCallback = nullptr;
    juce::BigInteger activeInputChannels;
    juce::BigInteger activeOutputChannels;
    double currentSampleRate = 0.0;
    int currentBufferSize = 0;
    bool isOpen_ = false;
    bool isPlaying_ = false;
    std::atomic<bool> isDestroying{false};
    std::atomic<int> activeCallbackCount{0};
    juce::CriticalSection lock;

    juce::AudioBuffer<float> tempOutputBuffer;
    juce::Array<const float*> activeInputPtrs;
    std::vector<float*> activeOutputPtrs{32};
};

} // namespace atk
