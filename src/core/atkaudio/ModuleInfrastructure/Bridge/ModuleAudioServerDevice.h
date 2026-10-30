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
        , actualDeviceKey(deviceType + "|" + actualDeviceName)
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            lastError = server->prepareDevice(actualDeviceKey);
        else
            lastError = "AudioServer is not available";
    }

    ~ModuleAudioServerDevice() override
    {
        isDestroying.store(true, std::memory_order_release);

        if (auto* server = AudioServer::getInstanceWithoutCreating())
            server->unregisterDirectCallback(actualDeviceKey, this);

        // Wait for any active callbacks to exit
        while (activeCallbackCount.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();

        close();
    }

    juce::StringArray getOutputChannelNames() override
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getDeviceChannelNames(actualDeviceKey, false);
        return {};
    }

    juce::StringArray getInputChannelNames() override
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getDeviceChannelNames(actualDeviceKey, true);
        return {};
    }

    juce::Array<double> getAvailableSampleRates() override
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getAvailableSampleRates(actualDeviceKey);
        return {};
    }

    juce::Array<int> getAvailableBufferSizes() override
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getAvailableBufferSizes(actualDeviceKey);
        return {};
    }

    int getDefaultBufferSize() override
    {
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            return server->getDefaultBufferSize(actualDeviceKey);
        return 0;
    }

    juce::String open(
        const juce::BigInteger& inputChannels,
        const juce::BigInteger& outputChannels,
        double sampleRate,
        int bufferSizeSamples
    ) override
    {
        auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
        if (!messageManager || !messageManager->isThisTheMessageThread())
            return "Audio device opening requires the message thread";

        double sampleRateChange = 0.0;
        int bufferSizeChange = 0;
        {
            const juce::ScopedLock sl(lock);
            // JUCE reuses the current channel setup for manual format edits.
            // New attachments and channel changes must inherit the hardware format.
            if (isOpen_
                && (inputChannels == requestedInputChannels || inputChannels == activeInputChannels)
                && (outputChannels == requestedOutputChannels || outputChannels == activeOutputChannels))
            {
                if (sampleRate > 0.0 && sampleRate != currentSampleRate)
                    sampleRateChange = sampleRate;
                if (bufferSizeSamples > 0 && bufferSizeSamples != currentBufferSize)
                    bufferSizeChange = bufferSizeSamples;
            }
        }

        close();
        lastError.clear();
        auto* server = AudioServer::getInstanceWithoutCreating();
        if (!server)
        {
            lastError = "AudioServer is not available";
            return lastError;
        }

        if ((sampleRateChange > 0.0 && !server->setDeviceSampleRate(actualDeviceKey, sampleRateChange))
            || (bufferSizeChange > 0 && !server->setDeviceBufferSize(actualDeviceKey, bufferSizeChange)))
        {
            lastError = "The requested hardware setting is not available";
            return lastError;
        }

        requestedInputChannels = inputChannels;
        requestedOutputChannels = outputChannels;
        lastError =
            server->openDeviceForCallback(actualDeviceKey, this, !inputChannels.isZero(), !outputChannels.isZero());
        if (lastError.isNotEmpty())
            return lastError;

        const juce::ScopedLock sl(lock);
        isOpen_ = true;
        return {};
    }

    void close() override
    {
        if (!isOpen_)
            return;

        stop();

        if (auto* server = AudioServer::getInstanceWithoutCreating())
            server->unregisterDirectCallback(actualDeviceKey, this);

        {
            const juce::ScopedLock sl(lock);
            isOpen_ = false;
            nativeRunning = false;
            currentSampleRate = 0.0;
            currentBufferSize = 0;
            activeInputChannels.clear();
            activeOutputChannels.clear();
        }
    }

    bool isOpen() override
    {
        const juce::ScopedLock sl(lock);
        return isOpen_ && nativeRunning;
    }

    void start(juce::AudioIODeviceCallback* newCallback) override
    {
        if (newCallback == nullptr)
            return;
        if (!isOpen())
        {
            lastError = "Audio device is not open";
            return;
        }

        stop();

        if (coordinator && !coordinator->tryActivate(this))
        {
            lastError = "Another audio device is already active";
            return;
        }

        {
            const juce::ScopedLock sl(lock);
            lastError.clear();
            userCallback = newCallback;
            isPlaying_ = true;

            auto inputCapacity = activeInputChannels.countNumberOfSetBits();
            auto outputCapacity = activeOutputChannels.countNumberOfSetBits();
            tempOutputBuffer.setSize(outputCapacity, currentBufferSize, false, false, true);
            activeOutputPtrs.resize(outputCapacity);
            activeInputPtrs.reserve(inputCapacity);

            if (userCallback != nullptr && nativeRunning && currentSampleRate > 0.0 && currentBufferSize > 0)
            {
                userCallback->audioDeviceAboutToStart(this);
                formatReady = true;
            }
        }
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
            callbackToStop = formatReady ? userCallback : nullptr;
            userCallback = nullptr;
            isPlaying_ = false;
            formatReady = false;
        }

        if (callbackToStop != nullptr)
            callbackToStop->audioDeviceStopped();
    }

    bool isPlaying() override
    {
        const juce::ScopedLock sl(lock);
        return isPlaying_ && nativeRunning && formatReady;
    }

    juce::String getLastError() override
    {
        const juce::ScopedLock sl(lock);
        return lastError;
    }

    int getCurrentBufferSizeSamples() override
    {
        const juce::ScopedLock sl(lock);
        if (currentBufferSize > 0)
            return currentBufferSize;
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            if (server->getPreparedDeviceSetup(actualDeviceKey, setup))
                return setup.bufferSize;
        return 0;
    }

    double getCurrentSampleRate() override
    {
        const juce::ScopedLock sl(lock);
        if (currentSampleRate > 0.0)
            return currentSampleRate;
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            if (server->getPreparedDeviceSetup(actualDeviceKey, setup))
                return setup.sampleRate;
        return 0.0;
    }

    int getCurrentBitDepth() override
    {
        return 16;
    }

    juce::BigInteger getActiveInputChannels() const override
    {
        const juce::ScopedLock sl(lock);
        return activeInputChannels;
    }

    juce::BigInteger getActiveOutputChannels() const override
    {
        const juce::ScopedLock sl(lock);
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
        // Early exit if destroying
        if (isDestroying.load(std::memory_order_acquire))
            return;

        // Track active callbacks for safe destruction
        activeCallbackCount.fetch_add(1, std::memory_order_acquire);

        struct Guard
        {
            std::atomic<int>& c;

            ~Guard()
            {
                c.fetch_sub(1, std::memory_order_release);
            }
        } guard{activeCallbackCount};

        if (isDestroying.load(std::memory_order_acquire))
            return;

        auto clearOutputs = [&]()
        {
            if (outputChannelData != nullptr)
                for (int ch = 0; ch < numOutputChannels; ++ch)
                    if (outputChannelData[ch] != nullptr)
                        juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
        };

        // Check if we're the active device
        if (!coordinator || !coordinator->isActive(this))
            return clearOutputs();

        const juce::ScopedLock sl(lock);

        if (!isOpen_ || !isPlaying_ || !formatReady || userCallback == nullptr || numSamples <= 0)
            return clearOutputs();

        // Count active channels within available range
        int numActiveInputs = 0;
        for (int ch = 0; ch < numInputChannels; ++ch)
            if (activeInputChannels[ch])
                ++numActiveInputs;

        int numActiveOutputs = 0;
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (activeOutputChannels[ch])
                ++numActiveOutputs;

        if (tempOutputBuffer.getNumChannels() < numActiveOutputs
            || tempOutputBuffer.getNumSamples() < numSamples
            || static_cast<int>(activeOutputPtrs.size()) < numActiveOutputs
            || static_cast<int>(activeInputPtrs.capacity()) < numActiveInputs)
        {
            return clearOutputs();
        }

        // Build filtered input pointers
        activeInputPtrs.clear();
        for (int ch = 0; ch < numInputChannels; ++ch)
            if (activeInputChannels[ch] && inputChannelData != nullptr && inputChannelData[ch] != nullptr)
                activeInputPtrs.push_back(inputChannelData[ch]);

        // Channel count mismatch - skip (will resync on next audioDeviceAboutToStart)
        if (activeInputPtrs.size() != numActiveInputs)
            return clearOutputs();

        // Build output pointers from temp buffer
        for (int i = 0; i < numActiveOutputs; ++i)
            activeOutputPtrs[i] = tempOutputBuffer.getWritePointer(i);

        // Call user callback
        userCallback->audioDeviceIOCallbackWithContext(
            activeInputPtrs.data(),
            numActiveInputs,
            activeOutputPtrs.data(),
            numActiveOutputs,
            numSamples,
            context
        );

        // Copy output back to hardware channels
        int activeIdx = 0;
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannelData == nullptr || outputChannelData[ch] == nullptr)
                continue;

            if (activeOutputChannels[ch] && activeIdx < numActiveOutputs)
                std::copy_n(tempOutputBuffer.getReadPointer(activeIdx++), numSamples, outputChannelData[ch]);
            else
                juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
        }
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        const juce::ScopedLock sl(lock);
        formatReady = false;
        nativeRunning = true;

        auto deviceInputs = device->getActiveInputChannels();
        auto deviceOutputs = device->getActiveOutputChannels();
        activeInputChannels = requestedInputChannels;
        activeOutputChannels = requestedOutputChannels;
        activeInputChannels &= deviceInputs;
        activeOutputChannels &= deviceOutputs;

        currentSampleRate = device->getCurrentSampleRate();
        currentBufferSize = device->getCurrentBufferSizeSamples();

        tempOutputBuffer.setSize(deviceOutputs.countNumberOfSetBits(), currentBufferSize, false, false, true);
        activeOutputPtrs.resize(deviceOutputs.countNumberOfSetBits());
        activeInputPtrs.reserve(deviceInputs.countNumberOfSetBits());

        if (userCallback != nullptr && isPlaying_)
        {
            userCallback->audioDeviceAboutToStart(this);
            formatReady = true;
        }
    }

    void audioDeviceStopped() override
    {
        const juce::ScopedLock sl(lock);
        nativeRunning = false;
        bool wasReady = formatReady;
        formatReady = false;
        if (userCallback != nullptr && wasReady)
            userCallback->audioDeviceStopped();
    }

    juce::String actualDeviceName;
    juce::String deviceType;
    juce::String actualDeviceKey;
    juce::String lastError;
    juce::AudioIODeviceCallback* userCallback = nullptr;
    juce::BigInteger requestedInputChannels;
    juce::BigInteger requestedOutputChannels;
    juce::BigInteger activeInputChannels;
    juce::BigInteger activeOutputChannels;
    double currentSampleRate = 0.0;
    int currentBufferSize = 0;
    bool isOpen_ = false;
    bool isPlaying_ = false;
    bool nativeRunning = false;
    bool formatReady = false;
    std::atomic<bool> isDestroying{false};
    std::atomic<int> activeCallbackCount{0};
    juce::CriticalSection lock;

    juce::AudioBuffer<float> tempOutputBuffer;
    std::vector<const float*> activeInputPtrs;
    std::vector<float*> activeOutputPtrs{32};
};

} // namespace atk
