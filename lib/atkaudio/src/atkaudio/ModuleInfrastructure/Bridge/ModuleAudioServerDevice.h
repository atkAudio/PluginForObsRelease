#pragma once

#include "ModuleAudioDevice.h"
#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <thread>

namespace atk
{

/**
 * Lightweight info about an AudioServer device (just name and type)
 */
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

/**
 * AudioIODevice bridge for AudioServer devices
 *
 * These work just like OBS Audio - they're real usable devices that bridge
 * between AudioServer and JUCE's AudioDeviceManager.
 *
 * Audio flow: AudioServer device → directCallback → JUCE callback → Module Graph
 *
 * This is a reusable component that can be used by any module implementation.
 */
class ModuleAudioServerDevice
    : public juce::AudioIODevice
    , public juce::AudioIODeviceCallback // AudioServer calls us via this interface
{
private:
    // Device coordinator for this module instance
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

    // AudioIODevice interface - Channel queries
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

    // AudioIODevice interface - Open/Close
    juce::String open(
        const juce::BigInteger& inputChannels,
        const juce::BigInteger& outputChannels,
        double sampleRate,
        int bufferSizeSamples
    ) override
    {
        DBG("ModuleAudioServerDevice::open() called for '" + actualDeviceName + "'");

        // Check if device is already open with different parameters - need to reopen
        bool needsReopen = false;

        if (isOpen_)
        {
            const juce::ScopedLock sl(lock);

            // Check if sample rate or buffer size changed - these require device reopen
            if ((sampleRate > 0.0 && !juce::exactlyEqual(currentSampleRate, sampleRate))
                || (bufferSizeSamples > 0 && currentBufferSize != bufferSizeSamples))
            {
                DBG("ModuleAudioServerDevice::open() parameters changed - reopening device");
                DBG("  Current: "
                    + juce::String(currentSampleRate)
                    + "Hz, "
                    + juce::String(currentBufferSize)
                    + " samples");
                DBG("  Requested: " + juce::String(sampleRate) + "Hz, " + juce::String(bufferSizeSamples) + " samples");
                needsReopen = true;
            }
        }

        // Update active channel configuration:
        // - On first open: ALWAYS enable ALL channels (ignore passed channels - they may be from previous device)
        // - When only channels changed (no rate/buffer change): use passed channels
        // - When reopening for rate/buffer: PRESERVE existing channels (ignore passed ones)
        if (!isOpen_ || !needsReopen)
        {
            const juce::ScopedLock sl(lock);

            // Get actual device channel counts from AudioServer
            int actualInputChannels = 0;
            int actualOutputChannels = 0;
            if (auto* server = AudioServer::getInstanceWithoutCreating())
            {
                actualInputChannels = server->getDeviceNumChannels(actualDeviceName, true);
                actualOutputChannels = server->getDeviceNumChannels(actualDeviceName, false);
            }

            // Store UI channel selection (for filtering in audio callback)
            activeInputChannels.clear();
            activeOutputChannels.clear();

            if (!isOpen_)
            {
                // First open: ALWAYS enable all channels by default
                activeInputChannels.setRange(0, actualInputChannels, true);
                activeOutputChannels.setRange(0, actualOutputChannels, true);

                DBG("ModuleAudioServerDevice::open() first open - enabling ALL channels by default");
            }
            else
            {
                // Device already open, only channels changed - use passed channels
                bool anyInputChannelRequested = false;
                bool anyOutputChannelRequested = false;

                for (int i = 0; i < actualInputChannels; ++i)
                {
                    if (inputChannels[i])
                    {
                        activeInputChannels.setBit(i);
                        anyInputChannelRequested = true;
                    }
                }

                for (int i = 0; i < actualOutputChannels; ++i)
                {
                    if (outputChannels[i])
                    {
                        activeOutputChannels.setBit(i);
                        anyOutputChannelRequested = true;
                    }
                }

                // If no specific channels requested, enable all channels
                if (!anyInputChannelRequested && actualInputChannels > 0)
                    activeInputChannels.setRange(0, actualInputChannels, true);

                if (!anyOutputChannelRequested && actualOutputChannels > 0)
                    activeOutputChannels.setRange(0, actualOutputChannels, true);
            }

            DBG("ModuleAudioServerDevice::open() channel configuration:");
            DBG("  Hardware has " + juce::String(actualInputChannels) + " input channels (all open)");
            DBG("  Hardware has " + juce::String(actualOutputChannels) + " output channels (all open)");
            DBG("  UI will use input channels: " + activeInputChannels.toString(2));
            DBG("  UI will use output channels: " + activeOutputChannels.toString(2));

            // Don't store JUCE's passed parameters yet - they may be from a different device
            // We'll query the actual parameters from AudioServer after opening
        }
        else
        {
            // Reopening for rate/buffer change - preserve existing channel selection
            const juce::ScopedLock sl(lock);
            DBG("ModuleAudioServerDevice::open() reopening - preserving existing active channels:");
            DBG("  Active input: " + activeInputChannels.toString(2));
            DBG("  Active output: " + activeOutputChannels.toString(2));
        }

        // Only interact with AudioServer if we need to reopen or if it's the first open
        if (needsReopen || !isOpen_)
        {
            juce::AudioDeviceManager::AudioDeviceSetup currentSetup;
            bool hasCurrentSetup = false;
            if (needsReopen)
            {
                if (auto* server = AudioServer::getInstanceWithoutCreating())
                {
                    hasCurrentSetup = server->getCurrentDeviceSetup(actualDeviceName, currentSetup);
                    if (hasCurrentSetup)
                    {
                        DBG("ModuleAudioServerDevice::open() retrieved current setup before closing:");
                        DBG("  Sample rate: " + juce::String(currentSetup.sampleRate));
                        DBG("  Buffer size: " + juce::String(currentSetup.bufferSize));
                    }
                }
                close();
            }

            // Register with AudioServer (outside lock to avoid deadlock)
            if (!isOpen_)
            {
                if (auto* server = AudioServer::getInstance())
                {
                    juce::AudioDeviceManager::AudioDeviceSetup setup;

                    if (needsReopen && hasCurrentSetup)
                    {
                        // Reopening - use current rate/buffer, but update what changed
                        setup.sampleRate = (sampleRate > 0.0 && !juce::exactlyEqual(currentSampleRate, sampleRate))
                                             ? sampleRate
                                             : currentSetup.sampleRate;
                        setup.bufferSize = (bufferSizeSamples > 0 && currentBufferSize != bufferSizeSamples)
                                             ? bufferSizeSamples
                                             : currentSetup.bufferSize;
                        // Don't pass channel configuration - AudioServer always opens all channels

                        DBG("ModuleAudioServerDevice::open() reopening with modified parameters");
                    }
                    else
                    {
                        // First open - use device defaults (ignore JUCE's passed values from previous device)
                        setup.sampleRate = 0.0;
                        setup.bufferSize = 0;
                        // Don't pass channel configuration - AudioServer always opens all channels
                        DBG("ModuleAudioServerDevice::open() first open - requesting device defaults");
                    }

                    DBG("ModuleAudioServerDevice::open() registering callback with AudioServer...");
                    DBG("  Passing to AudioServer: sampleRate="
                        + juce::String(setup.sampleRate)
                        + ", bufferSize="
                        + juce::String(setup.bufferSize));

                    if (!server->registerDirectCallback(actualDeviceName.toStdString(), this, setup))
                    {
                        DBG("ModuleAudioServerDevice::open() FAILED to register callback");
                        return "Failed to register with AudioServer";
                    }

                    // Now query the ACTUAL parameters that AudioServer opened the device with
                    const juce::ScopedLock sl(lock);
                    currentSampleRate = server->getCurrentSampleRate(actualDeviceName);
                    currentBufferSize = server->getCurrentBufferSize(actualDeviceName);

                    DBG("ModuleAudioServerDevice::open() successfully registered callback");
                    DBG("  Actual device parameters: sampleRate="
                        + juce::String(currentSampleRate)
                        + "Hz, bufferSize="
                        + juce::String(currentBufferSize)
                        + " samples");
                }

                isOpen_ = true;
            }
        }
        else
        {
            // Device already open, only channels changed - no need to interact with AudioServer
            DBG("ModuleAudioServerDevice::open() device already open with correct parameters - only updated channel "
                "selection");
        }

        DBG("ModuleAudioServerDevice::open() completed - isOpen=" + juce::String(isOpen_ ? "true" : "false"));
        return {};
    }

    void close() override
    {
        if (!isOpen_)
            return;

        DBG("ModuleAudioServerDevice::close() called for '" + actualDeviceName + "'");

        stop();

        // Unregister from AudioServer
        if (auto* server = AudioServer::getInstanceWithoutCreating())
        {
            DBG("ModuleAudioServerDevice::close() unregistering callback from AudioServer...");
            server->unregisterDirectCallback(actualDeviceName.toStdString(), this);
        }

        {
            const juce::ScopedLock sl(lock);
            // Don't clear activeInputChannels/activeOutputChannels - they're UI state that should persist
            isOpen_ = false;
        }

        DBG("ModuleAudioServerDevice::close() completed");
    }

    bool isOpen() override
    {
        return isOpen_;
    }

    // AudioIODevice interface - Start/Stop
    void start(juce::AudioIODeviceCallback* newCallback) override
    {
        DBG("ModuleAudioServerDevice::start() called for '" + actualDeviceName + "'");

        if (!isOpen_ || newCallback == nullptr)
        {
            DBG("ModuleAudioServerDevice::start() early return - not open or null callback");
            return;
        }

        stop();

        // Try to become the active device
        if (coordinator && !coordinator->tryActivate(this))
        {
            DBG("ModuleAudioServerDevice::start() failed to activate - another device is active");
            return;
        }

        {
            const juce::ScopedLock sl(lock);
            userCallback = newCallback;
            isPlaying_ = true;
        }

        if (userCallback != nullptr)
            userCallback->audioDeviceAboutToStart(this);

        DBG("ModuleAudioServerDevice::start() completed - now playing");
    }

    void stop() override
    {
        if (!isPlaying_)
            return;

        DBG("ModuleAudioServerDevice::stop() called for '" + actualDeviceName + "'");

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

        DBG("ModuleAudioServerDevice::stop() completed - stopped playing");
    }

    bool isPlaying() override
    {
        return isPlaying_;
    }

    // AudioIODevice interface - Status
    juce::String getLastError() override
    {
        return {};
    }

    int getCurrentBufferSizeSamples() override
    {
        return currentBufferSize;
    }

    double getCurrentSampleRate() override
    {
        return currentSampleRate;
    }

    int getCurrentBitDepth() override
    {
        return 16;
    }

    juce::BigInteger getActiveInputChannels() const override
    {
        // Return the user's UI channel selection
        // This is what JUCE will use for displaying/editing channel selection
        // and what the CallbackMaxSizeEnforcer will use to size its arrays
        return activeInputChannels;
    }

    juce::BigInteger getActiveOutputChannels() const override
    {
        // Return the user's UI channel selection
        // This is what JUCE will use for displaying/editing channel selection
        // and what the CallbackMaxSizeEnforcer will use to size its arrays
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
    // AudioIODeviceCallback interface - called by AudioServer
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
        for (int ch = 0; ch < numOutputChannels && activeIdx < numActiveOutputs; ++ch)
        {
            if (activeOutputChannels[ch] && outputChannelData[ch] != nullptr)
            {
                // Safety check: ensure we have valid buffer pointers
                const float* srcPtr = tempOutputBuffer.getReadPointer(activeIdx);
                if (srcPtr != nullptr && activeIdx < tempOutputBuffer.getNumChannels())
                {
                    std::memcpy(outputChannelData[ch], srcPtr, numSamples * sizeof(float));
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
    juce::BigInteger activeInputChannels;  // UI selection - which channels to actually use
    juce::BigInteger activeOutputChannels; // UI selection - which channels to actually use
    double currentSampleRate = 0.0;
    int currentBufferSize = 0;
    bool isOpen_ = false;
    bool isPlaying_ = false;
    std::atomic<bool> isDestroying{false};
    std::atomic<int> activeCallbackCount{0};
    juce::CriticalSection lock;

    // Temp buffers for channel filtering in audio callback
    // AudioServer passes ALL hardware channels, but JUCE's CallbackMaxSizeEnforcer
    // expects only the active channels, so we filter before/after the user callback
    juce::AudioBuffer<float> tempOutputBuffer;
    juce::Array<const float*> activeInputPtrs;
    std::vector<float*> activeOutputPtrs{32}; // Pre-allocate for up to 32 channels
};

} // namespace atk
