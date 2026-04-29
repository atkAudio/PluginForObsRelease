#pragma once

#include "ModuleAudioIODeviceType.h"
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <memory>

namespace atk
{

class ModuleDeviceManager : public juce::ChangeListener
{
public:
    ModuleDeviceManager(
        std::unique_ptr<ModuleAudioIODeviceType> deviceType,
        juce::AudioDeviceManager& deviceManager,
        MidiClient* externalMidiClient = nullptr
    )
        : customDeviceType(deviceType.release())
        , audioDeviceManager(deviceManager)
        , externalMidiClientPtr(externalMidiClient)
    {
        if (!externalMidiClientPtr)
            internalMidiClient = std::make_unique<MidiClient>();
    }

    ~ModuleDeviceManager()
    {
        cleanup();
    }

    bool initialize()
    {
        audioDeviceManager.addAudioDeviceType(std::unique_ptr<juce::AudioIODeviceType>(customDeviceType.release()));

        auto error = audioDeviceManager.initialise(256, 256, nullptr, true);
        if (error.isNotEmpty())
            return false;

        audioDeviceManager.addChangeListener(this);
        return true;
    }

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
            if (auto* currentDevice = audioDeviceManager.getCurrentAudioDevice())
            {
                if (currentDevice->getName() == "OBS Audio")
                    obsDevice.store(dynamic_cast<ModuleOBSAudioDevice*>(currentDevice), std::memory_order_release);
            }
            return true;
        }

        return false;
    }

    void processExternalAudio(float** buffer, int numChannels, int numSamples, double sampleRate)
    {
        ModuleOBSAudioDevice* device = obsDevice.load(std::memory_order_acquire);

        if (device != nullptr && device == audioDeviceManager.getCurrentAudioDevice())
        {
            try
            {
                outputBuffer.setSize(numChannels, numSamples, false, false, true);

                device->processExternalAudio(
                    const_cast<const float**>(buffer),
                    numChannels,
                    outputBuffer.getArrayOfWritePointers(),
                    numChannels,
                    numSamples,
                    sampleRate
                );

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy(
                        outputBuffer.getReadPointer(ch),
                        outputBuffer.getReadPointer(ch) + numSamples,
                        buffer[ch]
                    );

                return;
            }
            catch (...)
            {
            }
        }

        for (int ch = 0; ch < numChannels; ++ch)
            std::fill(buffer[ch], buffer[ch] + numSamples, 0.0f);
    }

    MidiClient& getMidiClient()
    {
        return externalMidiClientPtr ? *externalMidiClientPtr : *internalMidiClient;
    }

    juce::AudioDeviceManager& getAudioDeviceManager()
    {
        return audioDeviceManager;
    }

    ModuleOBSAudioDevice* getOBSDevice()
    {
        return obsDevice.load(std::memory_order_acquire);
    }

    void cleanup()
    {
        if (cleanedUp)
            return;
        cleanedUp = true;

        obsDevice.store(nullptr, std::memory_order_release);

        auto* mm = juce::MessageManager::getInstance();
        if (!mm)
            return;

        if (mm->isThisTheMessageThread())
            audioDeviceManager.removeChangeListener(this);
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override
    {
        juce::ignoreUnused(source);

        // Update obsDevice pointer based on current device
        auto* currentDevice = audioDeviceManager.getCurrentAudioDevice();

        if (currentDevice != nullptr && currentDevice->getName() == "OBS Audio")
        {
            auto* obsPtr = dynamic_cast<ModuleOBSAudioDevice*>(currentDevice);
            obsDevice.store(obsPtr, std::memory_order_release);
        }
        else
        {
            obsDevice.store(nullptr, std::memory_order_release);
        }
    }

    std::unique_ptr<ModuleAudioIODeviceType> customDeviceType;
    juce::AudioDeviceManager& audioDeviceManager;

    // MIDI client can be external (referenced) or internal (owned)
    MidiClient* externalMidiClientPtr = nullptr;
    std::unique_ptr<MidiClient> internalMidiClient;

    std::atomic<ModuleOBSAudioDevice*> obsDevice{nullptr};
    juce::AudioBuffer<float> outputBuffer{16, 8192};
    bool cleanedUp = false;
};

} // namespace atk
