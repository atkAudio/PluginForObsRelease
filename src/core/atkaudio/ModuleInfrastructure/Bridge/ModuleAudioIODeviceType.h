#pragma once

#include "ModuleAudioDevice.h"
#include "ModuleAudioServerDevice.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <set>

namespace atk
{

class ModuleAudioIODeviceType : public juce::AudioIODeviceType
{
public:
    ModuleAudioIODeviceType(const juce::String& typeName = "Module Audio")
        : juce::AudioIODeviceType(typeName)
        , coordinator(std::make_shared<ModuleDeviceCoordinator>())
    {
    }

    ~ModuleAudioIODeviceType() override = default;

    void scanForDevices() override
    {
        deviceNames.clear();
        audioServerDevices.clear();

        if (shouldIncludeOBSAudio())
            deviceNames.add("OBS Audio");

        if (auto* audioServer = AudioServer::getInstanceWithoutCreating())
        {
            auto inputDevicesByType = audioServer->getInputDevicesByType();
            auto outputDevicesByType = audioServer->getOutputDevicesByType();

            std::set<juce::String> deviceTypes;
            for (const auto& pair : inputDevicesByType)
                deviceTypes.insert(pair.first);
            for (const auto& pair : outputDevicesByType)
                deviceTypes.insert(pair.first);

            const juce::StringArray allowedTypes = {"ASIO", "CoreAudio", "ALSA", "Windows Audio"};

            for (const auto& deviceType : deviceTypes)
            {
                if (!allowedTypes.contains(deviceType))
                    continue;

                std::set<juce::String> devicesForType;

                auto inputIt = inputDevicesByType.find(deviceType);
                if (inputIt != inputDevicesByType.end())
                    for (const auto& device : inputIt->second)
                        devicesForType.insert(device);

                auto outputIt = outputDevicesByType.find(deviceType);
                if (outputIt != outputDevicesByType.end())
                    for (const auto& device : outputIt->second)
                        devicesForType.insert(device);

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
        return -1; // No default device - never auto-select
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

        for (const auto& info : audioServerDevices)
            if (info.getDisplayName() == deviceName)
                return createAudioServerDevice(deviceName, info);

        return nullptr;
    }

protected:
    virtual bool shouldIncludeOBSAudio() const
    {
        return true;
    }

    virtual juce::AudioIODevice* createOBSDevice(const juce::String& deviceName)
    {
        return new ModuleOBSAudioDevice(deviceName, coordinator, getTypeName());
    }

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
