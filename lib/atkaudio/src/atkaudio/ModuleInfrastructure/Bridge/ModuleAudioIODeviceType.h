#pragma once

#include "ModuleAudioDevice.h"
#include "ModuleAudioServerDevice.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <set>

namespace atk
{

/**
 * AudioIODeviceType for Module Audio devices
 *
 * This manages both:
 * - OBS Audio device (for processing OBS audio through the module)
 * - AudioServer devices (ASIO, CoreAudio, ALSA, Windows Audio)
 *
 * Each device type has its own ModuleDeviceCoordinator instance. The coordinator
 * ensures only one device is active **within this module instance**. Multiple
 * module instances can coexist, each registering their active device with AudioServer
 * for concurrent hardware callbacks.
 *
 * Example:
 * - Module A (PluginHost2): OBS Audio active, registered callback with AudioServer
 * - Module B (PluginHost3): ASIO Device active, registered callback with AudioServer
 * Both process audio concurrently via their independent AudioServer callbacks.
 *
 * This is a reusable component that can be used by any module implementation
 * (PluginHost2, PluginHost3, etc.). Derived classes can customize the device
 * creation by overriding createOBSDevice() and createAudioServerDevice().
 */
class ModuleAudioIODeviceType : public juce::AudioIODeviceType
{
public:
    ModuleAudioIODeviceType(const juce::String& typeName = "Module Audio")
        : juce::AudioIODeviceType(typeName)
        , coordinator(std::make_shared<ModuleDeviceCoordinator>())
    {
    }

    void scanForDevices() override
    {
        deviceNames.clear();
        audioServerDevices.clear();

        // Always include OBS Audio
        deviceNames.add("OBS Audio");

        // Query AudioServer for available devices
        if (auto* audioServer = AudioServer::getInstanceWithoutCreating())
        {
            // Get devices grouped by type
            auto inputDevicesByType = audioServer->getInputDevicesByType();
            auto outputDevicesByType = audioServer->getOutputDevicesByType();

            // Merge input and output device types
            std::set<juce::String> deviceTypes;
            for (const auto& pair : inputDevicesByType)
                deviceTypes.insert(pair.first);
            for (const auto& pair : outputDevicesByType)
                deviceTypes.insert(pair.first);

            // Only include professional audio interfaces
            const juce::StringArray allowedTypes = {"ASIO", "CoreAudio", "ALSA", "Windows Audio"};

            for (const auto& deviceType : deviceTypes)
            {
                if (!allowedTypes.contains(deviceType))
                    continue;

                // Collect all unique device names for this type
                std::set<juce::String> devicesForType;

                auto inputIt = inputDevicesByType.find(deviceType);
                if (inputIt != inputDevicesByType.end())
                    for (const auto& device : inputIt->second)
                        devicesForType.insert(device);

                auto outputIt = outputDevicesByType.find(deviceType);
                if (outputIt != outputDevicesByType.end())
                    for (const auto& device : outputIt->second)
                        devicesForType.insert(device);

                // Add each device to our list
                for (const auto& deviceName : devicesForType)
                {
                    AudioServerDeviceInfo info;
                    info.deviceName = deviceName;
                    info.deviceType = deviceType;

                    audioServerDevices.add(info);
                    deviceNames.add(info.getDisplayName());
                }
            }
        }
    }

    juce::StringArray getDeviceNames(bool /*forInput*/) const override
    {
        return deviceNames;
    }

    int getDefaultDeviceIndex(bool /*forInput*/) const override
    {
        return 0; // OBS Audio is default
    }

    int getIndexOfDevice(juce::AudioIODevice* device, bool /*forInput*/) const override
    {
        if (device == nullptr)
            return -1;
        return deviceNames.indexOf(device->getName());
    }

    bool hasSeparateInputsAndOutputs() const override
    {
        return false;
    }

    juce::AudioIODevice*
    createDevice(const juce::String& outputDeviceName, const juce::String& inputDeviceName) override
    {
        const juce::String deviceName = outputDeviceName.isNotEmpty() ? outputDeviceName : inputDeviceName;

        if (deviceName == "OBS Audio")
            return createOBSDevice(deviceName);

        // Find the AudioServer device info
        for (const auto& info : audioServerDevices)
            if (info.getDisplayName() == deviceName)
                return createAudioServerDevice(deviceName, info);

        return nullptr;
    }

protected:
    /**
     * Create the OBS audio device
     * Override this in derived classes to create specialized OBS devices
     */
    virtual juce::AudioIODevice* createOBSDevice(const juce::String& deviceName)
    {
        return new ModuleOBSAudioDevice(deviceName, coordinator, getTypeName());
    }

    /**
     * Create an AudioServer device
     * Override this in derived classes to create specialized AudioServer devices
     */
    virtual juce::AudioIODevice*
    createAudioServerDevice(const juce::String& displayName, const AudioServerDeviceInfo& info)
    {
        return new ModuleAudioServerDevice(displayName, info.deviceName, info.deviceType, coordinator, getTypeName());
    }

private:
    std::shared_ptr<ModuleDeviceCoordinator> coordinator;
    juce::StringArray deviceNames;
    juce::Array<AudioServerDeviceInfo> audioServerDevices;
};

} // namespace atk
