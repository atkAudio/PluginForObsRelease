#pragma once

#include "../FifoBuffer2.h"
#include "../LookAndFeel.h"
#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleAudioIODeviceType.h>
#include <juce_audio_utils/juce_audio_utils.h>

class DeviceIoAudioDeviceType : public atk::ModuleAudioIODeviceType
{
public:
    DeviceIoAudioDeviceType()
        : atk::ModuleAudioIODeviceType("Hardware Audio")
    {
        atk::AudioServer::getInstance()->initialize();
    }

protected:
    bool shouldIncludeOBSAudio() const override
    {
        return false;
    }
};

class DeviceIoApp final
    : public juce::Component
    , public juce::AudioIODeviceCallback
    , public atk::AudioServer::Listener
    , private juce::ChangeListener
{
public:
    DeviceIoApp(int maxInputChannels, int maxOutputChannels)
        : maxInputChannels(maxInputChannels)
        , maxOutputChannels(maxOutputChannels)
    {
        deviceManager.addAudioDeviceType(std::make_unique<DeviceIoAudioDeviceType>());
        deviceManager.addAudioCallback(this);
        deviceManager.addChangeListener(this);

        if (auto* server = atk::AudioServer::getInstanceWithoutCreating())
            server->addListener(this);

        audioSettingsComp = std::make_unique<juce::AudioDeviceSelectorComponent>(
            deviceManager,
            0,
            maxInputChannels,
            0,
            maxOutputChannels,
            false,
            false,
            false,
            false
        );

        addAndMakeVisible(audioSettingsComp.get());
        updateSize();
    }

    ~DeviceIoApp() override
    {
        if (auto* server = atk::AudioServer::getInstanceWithoutCreating())
            server->removeListener(this);
        deviceManager.removeChangeListener(this);
        deviceManager.removeAudioCallback(this);
    }

    void audioServerDeviceListChanged() override
    {
        // Notify the JUCE device types about the change
        for (auto* type : deviceManager.getAvailableDeviceTypes())
            type->scanForDevices();

        auto* currentDevice = deviceManager.getCurrentAudioDevice();
        juce::String currentDeviceName = currentDevice ? currentDevice->getName() : juce::String();

        // Check if we had a device that's now gone
        if (currentDevice != nullptr)
        {
            bool deviceStillExists = false;
            for (auto* type : deviceManager.getAvailableDeviceTypes())
            {
                auto outputDevices = type->getDeviceNames(false);
                auto inputDevices = type->getDeviceNames(true);

                if (outputDevices.contains(currentDeviceName) || inputDevices.contains(currentDeviceName))
                {
                    deviceStillExists = true;
                    break;
                }
            }

            if (!deviceStillExists)
            {
                // Save current state for restoration later
                if (!currentDeviceName.isEmpty())
                {
                    pendingDeviceName = currentDeviceName;
                    pendingStateXml = getStateXml();
                    DBG("[Hotplug] Device disconnected, saving state for: " << pendingDeviceName);
                }
                // Close the device that's no longer available
                deviceManager.closeAudioDevice();
                return;
            }
        }

        // Check if we have a pending device to restore
        if (currentDevice == nullptr && !pendingDeviceName.isEmpty())
        {
            DBG("[Hotplug] Device reconnected, attempting restore: " << pendingDeviceName);
            tryRestorePendingDevice();
        }
    }

    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext&
    ) override
    {
        if (needsBufferClear.exchange(false))
        {
            toObsBuffer.reset();
            fromObsBuffer.reset();
            toObsBuffer.clearPrepared();
            fromObsBuffer.clearPrepared();
        }

        if (numInputChannels > 0 && inputChannelData != nullptr)
            toObsBuffer.write(inputChannelData, numInputChannels, numSamples, currentSampleRate);

        if (numOutputChannels > 0 && outputChannelData != nullptr)
            fromObsBuffer.read(outputChannelData, numOutputChannels, numSamples, currentSampleRate);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        if (device != nullptr)
        {
            currentSampleRate = device->getCurrentSampleRate();
            currentBufferSize = device->getCurrentBufferSizeSamples();
        }
        needsBufferClear.store(true);
    }

    void audioDeviceStopped() override
    {
        needsBufferClear.store(true);
    }

    void changeListenerCallback(juce::ChangeBroadcaster* source) override
    {
        if (source != &deviceManager || isRestoringState)
            return;

        if (audioSettingsComp)
            audioSettingsComp->resized();

        auto* device = deviceManager.getCurrentAudioDevice();

        if (device == nullptr)
            return;

        juce::String currentDeviceName = device->getName();

        if (currentDeviceName != lastDeviceName && !lastDeviceName.isEmpty())
        {
            pendingDeviceName.clear();
            pendingStateXml.clear();
        }

        lastDeviceName = currentDeviceName;
    }

    juce::String getCurrentDeviceName() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return device->getName();
        return {};
    }

    double getCurrentSampleRate() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return device->getCurrentSampleRate();
        return currentSampleRate;
    }

    int getCurrentBufferSize() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return device->getCurrentBufferSizeSamples();
        return currentBufferSize;
    }

    juce::BigInteger getActiveInputChannels() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return device->getActiveInputChannels();
        return {};
    }

    juce::BigInteger getActiveOutputChannels() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return device->getActiveOutputChannels();
        return {};
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        if (audioSettingsComp)
            audioSettingsComp->setBounds(getLocalBounds().reduced(margin));
    }

    void childBoundsChanged(juce::Component* child) override
    {
        if (child == audioSettingsComp.get())
            updateSize();
    }

    void updateSize()
    {
        if (audioSettingsComp)
        {
            int width = juce::jmax(450, audioSettingsComp->getWidth() + margin * 2);
            int height = juce::jmax(80, audioSettingsComp->getHeight() + margin * 2);
            setSize(width, height);
        }
    }

    auto& getFromObsBuffer()
    {
        return fromObsBuffer;
    }

    auto& getToObsBuffer()
    {
        return toObsBuffer;
    }

    juce::String getStateXml() const
    {
        auto state = deviceManager.createStateXml();
        return state ? state->toString() : juce::String();
    }

    void setStateXml(const juce::String& xmlString)
    {
        if (xmlString.isEmpty())
            return;

        auto xml = juce::parseXML(xmlString);
        if (!xml)
            return;

        auto* deviceSetup = xml->getChildByName("DEVICESETUP");
        if (!deviceSetup)
            return;

        juce::String savedOutputDevice = deviceSetup->getStringAttribute("audioOutputDeviceName");
        juce::String savedInputDevice = deviceSetup->getStringAttribute("audioInputDeviceName");
        pendingDeviceName = savedOutputDevice.isNotEmpty() ? savedOutputDevice : savedInputDevice;
        pendingStateXml = xmlString;

        tryRestorePendingDevice();
    }

    juce::AudioDeviceManager& getDeviceManager()
    {
        return deviceManager;
    }

private:
    void tryRestorePendingDevice()
    {
        if (pendingDeviceName.isEmpty() || pendingStateXml.isEmpty())
            return;

        if (deviceManager.getCurrentAudioDevice() != nullptr)
            return;

        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            type->scanForDevices();

            auto outputDevices = type->getDeviceNames(false);
            auto inputDevices = type->getDeviceNames(true);

            bool deviceFound = outputDevices.contains(pendingDeviceName) || inputDevices.contains(pendingDeviceName);

            if (!deviceFound)
                continue;

            if (auto xml = juce::parseXML(pendingStateXml))
            {
                // DEVICESETUP might be the root element or a child element
                juce::XmlElement* deviceSetup = nullptr;
                if (xml->hasTagName("DEVICESETUP"))
                    deviceSetup = xml.get();
                else
                    deviceSetup = xml->getChildByName("DEVICESETUP");

                if (deviceSetup != nullptr)
                {
                    isRestoringState = true;
                    auto error = deviceManager.initialise(maxInputChannels, maxOutputChannels, deviceSetup, false);
                    isRestoringState = false;

                    if (auto* device = deviceManager.getCurrentAudioDevice())
                    {
                        if (device->getName() == pendingDeviceName)
                        {
                            DBG("[Hotplug] Restored device: " << pendingDeviceName);
                            lastDeviceName = pendingDeviceName;
                            pendingDeviceName.clear();
                            pendingStateXml.clear();
                            return;
                        }
                        deviceManager.closeAudioDevice();
                    }
                }
            }
            return;
        }
    }

    static constexpr int margin = 10;

    int maxInputChannels = 0;
    int maxOutputChannels = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 512;
    std::atomic<bool> needsBufferClear{false};
    bool isRestoringState = false;
    juce::String lastDeviceName;
    juce::String pendingDeviceName;
    juce::String pendingStateXml;

    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> audioSettingsComp;

    SyncBuffer toObsBuffer;
    SyncBuffer fromObsBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceIoApp)
};

class AudioAppMainWindow final : public juce::DocumentWindow
{
public:
    AudioAppMainWindow(DeviceIoApp& app)
        : juce::DocumentWindow(
              "DeviceIo Audio Settings",
              juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
              DocumentWindow::allButtons
          )
        , deviceIoApp(app)
    {
        setTitleBarButtonsRequired(DocumentWindow::closeButton, false);
        setContentNonOwned(&app, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        removeFromDesktop();
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    DeviceIoApp& deviceIoApp;
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioAppMainWindow)
};
