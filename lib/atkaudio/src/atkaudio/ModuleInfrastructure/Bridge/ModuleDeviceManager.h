#pragma once

#include "ModuleAudioIODeviceType.h"
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <memory>

namespace atk
{

/**
 * Module Device Manager
 *
 * Self-contained composite that encapsulates the pattern of integrating a JUCE AudioDeviceManager with:
 * - Custom AudioIODeviceType (OBS Audio + AudioServer devices)
 * - MidiClient for MIDI I/O through MidiServer (internal by default, or external)
 * - Atomic device pointer for realtime-safe external audio processing
 *
 * This is a reusable component that handles the boilerplate of:
 * 1. Registering custom device type
 * 2. Initializing AudioDeviceManager
 * 3. Opening the OBS Audio device by default
 * 4. Tracking device changes via ChangeListener
 * 5. Providing realtime-safe access to the active OBS device
 * 6. Managing MIDI client lifecycle (internal or external)
 *
 * By default, ModuleDeviceManager is fully self-contained with its own MidiClient.
 * Optionally, you can provide an external MidiClient to share it with other components.
 *
 * Usage pattern (self-contained):
 * ```cpp
 * class MyModule {
 *     ModuleDeviceManager deviceManager;
 *
 *     MyModule() {
 *         deviceManager.initialize();
 *         deviceManager.openOBSDevice();
 *         // deviceManager has its own integrated MidiClient
 *     }
 *
 *     void process(float** buffer, int channels, int samples, double sampleRate) {
 *         deviceManager.processExternalAudio(buffer, channels, samples, sampleRate);
 *     }
 * };
 * ```
 *
 * Usage pattern (with external MidiClient):
 * ```cpp
 * class MyModule {
 *     MidiClient sharedMidiClient;
 *     ModuleDeviceManager deviceManager;
 *
 *     MyModule()
 *         : deviceManager(..., &sharedMidiClient) {
 *         // deviceManager.getMidiClient() returns reference to sharedMidiClient
 *     }
 * };
 * ```
 */
class ModuleDeviceManager : public juce::ChangeListener
{
public:
    /**
     * Constructor
     * @param deviceType Custom device type to use (takes ownership)
     * @param deviceManager JUCE AudioDeviceManager to manage
     * @param externalMidiClient Optional external MIDI client to use instead of internal one
     */
    ModuleDeviceManager(
        std::unique_ptr<ModuleAudioIODeviceType> deviceType,
        juce::AudioDeviceManager& deviceManager,
        MidiClient* externalMidiClient = nullptr
    )
        : customDeviceType(deviceType.release())
        , audioDeviceManager(deviceManager)
        , externalMidiClientPtr(externalMidiClient)
    {
        // If no external MIDI client provided, create internal one
        if (!externalMidiClientPtr)
            internalMidiClient = std::make_unique<MidiClient>();
    }

    ~ModuleDeviceManager()
    {
        cleanup();
    }

    /**
     * Initialize the device manager
     * - Registers custom device type
     * - Initializes AudioDeviceManager
     * - Sets up change listener
     */
    bool initialize()
    {
        // Register custom device type (transfer ownership)
        audioDeviceManager.addAudioDeviceType(std::unique_ptr<juce::AudioIODeviceType>(customDeviceType.release()));

        // Initialize device manager (don't specify device yet)
        auto error = audioDeviceManager.initialise(256, 256, nullptr, true);
        if (error.isNotEmpty())
            return false;

        // Register as change listener
        audioDeviceManager.addChangeListener(this);

        return true;
    }

    /**
     * Open the OBS Audio device
     * This should be called after initialize()
     */
    bool openOBSDevice()
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.outputDeviceName = "OBS Audio";
        setup.inputDeviceName = "OBS Audio";
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;

        auto error = audioDeviceManager.setAudioDeviceSetup(setup, true);
        if (error.isEmpty())
        {
            // Update the device pointer
            if (auto* currentDevice = audioDeviceManager.getCurrentAudioDevice())
            {
                // Check if it's an OBS device by name
                if (currentDevice->getName() == "OBS Audio")
                    obsDevice.store(dynamic_cast<ModuleOBSAudioDevice*>(currentDevice), std::memory_order_release);
            }
            return true;
        }

        return false;
    }

    /**
     * Process external audio from OBS through the active device
     * This is realtime-safe and can be called from any thread
     */
    void processExternalAudio(float** buffer, int numChannels, int numSamples, double sampleRate)
    {
        // Realtime-safe: Load the device pointer atomically
        ModuleOBSAudioDevice* device = obsDevice.load(std::memory_order_acquire);

        // Double-check device is still valid and matches current device
        // This prevents using a device that's being destroyed during device changes
        if (device != nullptr && device == audioDeviceManager.getCurrentAudioDevice())
        {
            try
            {
                // Prepare output buffer
                outputBuffer.setSize(numChannels, numSamples, false, false, true);

                // Call device to process
                device->processExternalAudio(
                    const_cast<const float**>(buffer),
                    numChannels,
                    outputBuffer.getArrayOfWritePointers(),
                    numChannels,
                    numSamples,
                    sampleRate
                );

                // Copy output back
                for (int ch = 0; ch < numChannels; ++ch)
                    std::memcpy(buffer[ch], outputBuffer.getReadPointer(ch), numSamples * sizeof(float));

                return;
            }
            catch (...)
            {
                // Device was destroyed during processing - fall through to silence
            }
        }

        // Device not OBS Audio or not initialized or being changed - output silence
        for (int ch = 0; ch < numChannels; ++ch)
            std::fill(buffer[ch], buffer[ch] + numSamples, 0.0f);
    }

    /**
     * Get the MIDI client (either external or internal)
     */
    MidiClient& getMidiClient()
    {
        return externalMidiClientPtr ? *externalMidiClientPtr : *internalMidiClient;
    }

    /**
     * Get the AudioDeviceManager
     */
    juce::AudioDeviceManager& getAudioDeviceManager()
    {
        return audioDeviceManager;
    }

    /**
     * Get the active OBS device (may be nullptr)
     * This is NOT realtime-safe - use processExternalAudio() for realtime processing
     */
    ModuleOBSAudioDevice* getOBSDevice()
    {
        return obsDevice.load(std::memory_order_acquire);
    }

    /**
     * Manual cleanup - removes change listener
     * Automatically called by destructor, but can be called early if needed
     * Safe to call multiple times
     */
    void cleanup()
    {
        // Make this idempotent - safe to call multiple times
        if (cleanedUp)
            return;
        cleanedUp = true;

        // Clear the device pointer immediately
        obsDevice.store(nullptr, std::memory_order_release);

        // Unregister change listener
        auto* mm = juce::MessageManager::getInstance();
        if (!mm)
        {
            // Message manager already destroyed, nothing we can do
            return;
        }

        if (mm->isThisTheMessageThread())
        {
            // We're on the message thread, remove directly
            audioDeviceManager.removeChangeListener(this);
        }
        else
        {
            // We're on a background thread during destruction
            // DO NOT use async dispatch - it will execute after AudioDeviceManager is destroyed
            // Just skip the removal - acceptable during shutdown since everything is being torn down
            // The listener list will be destroyed anyway when AudioDeviceManager destructs
        }
    }

private:
    // ChangeListener callback - runs on message thread
    void changeListenerCallback(juce::ChangeBroadcaster* source) override
    {
        juce::ignoreUnused(source);

        // Update obsDevice pointer based on current device
        auto* currentDevice = audioDeviceManager.getCurrentAudioDevice();

        if (currentDevice != nullptr && currentDevice->getName() == "OBS Audio")
        {
            // Switched to OBS Audio device
            auto* obsPtr = dynamic_cast<ModuleOBSAudioDevice*>(currentDevice);
            obsDevice.store(obsPtr, std::memory_order_release);
        }
        else
        {
            // Switched away from OBS Audio
            obsDevice.store(nullptr, std::memory_order_release);
        }
    }

    std::unique_ptr<ModuleAudioIODeviceType> customDeviceType;
    juce::AudioDeviceManager& audioDeviceManager;

    // MIDI client can be external (referenced) or internal (owned)
    MidiClient* externalMidiClientPtr = nullptr;
    std::unique_ptr<MidiClient> internalMidiClient;

    std::atomic<ModuleOBSAudioDevice*> obsDevice{nullptr};
    juce::AudioBuffer<float> outputBuffer{16, 8192}; // Max 16 channels, 8192 samples
    bool cleanedUp = false;                          // Flag to make cleanup idempotent
};

} // namespace atk
