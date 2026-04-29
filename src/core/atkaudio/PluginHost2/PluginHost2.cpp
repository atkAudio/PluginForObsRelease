#include "API/PluginHost2.h"

#include "UI/MainHostWindow.h"
#include <atkaudio/atkAudioModule.h>
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleBridge.h>
#include <obs-module.h>

atk::PluginHost2::PluginHost2()
    : mainHostWindow(std::make_unique<MainHostWindow>())
{
    moduleDeviceManager = std::make_unique<atk::ModuleDeviceManager>(
        std::make_unique<atk::ModuleAudioIODeviceType>("PluginHost2 Audio"),
        mainHostWindow->getDeviceManager()
    );

    if (moduleDeviceManager->initialize())
        moduleDeviceManager->openOBSDevice();

    mainHostWindow->setExternalMidiClient(moduleDeviceManager->getMidiClient());
    mainHostWindow->setVisible(false);
}

atk::PluginHost2::~PluginHost2()
{
    cancelPendingUpdate();

    auto* windowPtr = mainHostWindow.release();
    auto* deviceManagerPtr = moduleDeviceManager.release();

    atkAudioModule::destroyOnMessageThread(
        [windowPtr, deviceManagerPtr]()
        {
            if (windowPtr != nullptr)
                windowPtr->getDeviceManager().closeAudioDevice();

            if (deviceManagerPtr != nullptr)
            {
                deviceManagerPtr->cleanup();
                delete deviceManagerPtr;
            }

            delete windowPtr;
        }
    );
}

void atk::PluginHost2::handleAsyncUpdate()
{
    if (pendingStateString.isEmpty() || mainHostWindow == nullptr)
        return;

    if (mainHostWindow->graphHolder != nullptr && mainHostWindow->graphHolder->graph != nullptr)
    {
        mainHostWindow->graphHolder->graph->clear();

        auto xml = juce::XmlDocument::parse(pendingStateString);
        if (!xml)
        {
            pendingStateString.clear();
            return;
        }

        auto* savedState = xml->getChildByName("DEVICESETUP");
        auto& deviceManager = mainHostWindow->getDeviceManager();
        if (savedState)
            deviceManager.initialise(256, 256, savedState, true);

        juce::XmlElement* filterGraph = xml->getChildByName("FILTERGRAPH");
        if (filterGraph != nullptr)
            mainHostWindow->setGraphXml(*filterGraph);

        auto* audioServerElement = xml->getChildByName("AUDIOSERVER");
        if (audioServerElement)
        {
            auto* audioServer = atk::AudioServer::getInstance();

            DBG("PluginHost2: Restoring AudioServer device settings...");

            for (auto* deviceElement : audioServerElement->getChildIterator())
            {
                if (deviceElement->hasTagName("DEVICE"))
                {
                    juce::String deviceName = deviceElement->getStringAttribute("name");

                    if (deviceElement->hasAttribute("sampleRate"))
                    {
                        double sampleRate = deviceElement->getDoubleAttribute("sampleRate");
                        DBG("PluginHost2: Restoring sample rate for " + deviceName + " to " + juce::String(sampleRate));
                        audioServer->setDeviceSampleRate(deviceName, sampleRate);
                    }

                    if (deviceElement->hasAttribute("bufferSize"))
                    {
                        int bufferSize = deviceElement->getIntAttribute("bufferSize");
                        DBG("PluginHost2: Restoring buffer size for " + deviceName + " to " + juce::String(bufferSize));
                        audioServer->setDeviceBufferSize(deviceName, bufferSize);
                    }
                }
            }
        }

        auto* midiElement = xml->getChildByName("MIDISTATE");
        if (midiElement != nullptr)
        {
            atk::MidiClientState midiState;
            midiState.deserialize(midiElement->getStringAttribute("state"));
            mainHostWindow->getMidiClient().setSubscriptions(midiState);
        }
    }

    pendingStateString.clear();
}

void atk::PluginHost2::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    if (moduleDeviceManager != nullptr)
        moduleDeviceManager->processExternalAudio(buffer, numChannels, numSamples, sampleRate);
}

void atk::PluginHost2::getState(std::string& s)
{
    if (mainHostWindow == nullptr)
    {
        s.clear();
        return;
    }

    juce::XmlElement xml("atkAudioPluginHost2State");

    auto state = mainHostWindow->getDeviceManager().createStateXml();
    if (state != nullptr)
        xml.addChildElement(state.release());

    auto* audioServerElement = new juce::XmlElement("AUDIOSERVER");
    if (auto* audioServer = atk::AudioServer::getInstance())
    {
        auto inputDevices = audioServer->getAvailableInputDevices();
        auto outputDevices = audioServer->getAvailableOutputDevices();

        juce::StringArray allDevices;
        allDevices.addArray(inputDevices);
        for (const auto& dev : outputDevices)
            if (!allDevices.contains(dev))
                allDevices.add(dev);

        for (const auto& deviceName : allDevices)
        {
            double sampleRate = audioServer->getCurrentSampleRate(deviceName);
            int bufferSize = audioServer->getCurrentBufferSize(deviceName);

            if (sampleRate > 0.0 || bufferSize > 0)
            {
                auto* deviceElement = new juce::XmlElement("DEVICE");
                deviceElement->setAttribute("name", deviceName);
                if (sampleRate > 0.0)
                    deviceElement->setAttribute("sampleRate", sampleRate);
                if (bufferSize > 0)
                    deviceElement->setAttribute("bufferSize", bufferSize);
                audioServerElement->addChildElement(deviceElement);

                DBG("PluginHost2: Saving AudioServer device settings - "
                    + deviceName
                    + " sampleRate="
                    + juce::String(sampleRate)
                    + " bufferSize="
                    + juce::String(bufferSize));
            }
        }
    }
    xml.addChildElement(audioServerElement);

    auto* filterGraph = new juce::XmlElement("FILTERGRAPH");
    mainHostWindow->getGraphXml(*filterGraph);
    xml.addChildElement(filterGraph);

    auto midiState = mainHostWindow->getMidiClient().getSubscriptions();
    auto* midiElement = new juce::XmlElement("MIDISTATE");
    midiElement->setAttribute("state", midiState.serialize());
    xml.addChildElement(midiElement);

    s = xml.toString().toStdString();
}

void atk::PluginHost2::setState(std::string& s)
{
    if (s.empty())
        return;

    pendingStateString = juce::String(s);
    triggerAsyncUpdate();
}

juce::Component* atk::PluginHost2::getWindowComponent()
{
    return mainHostWindow.get();
}

void atk::PluginHost2::setParentSource(void* parentSource)
{
    auto* source = static_cast<obs_source_t*>(parentSource);
    if (mainHostWindow != nullptr && source != nullptr)
    {
        const char* uuid = obs_source_get_uuid(source);
        mainHostWindow->setParentSourceUuid(uuid ? uuid : "");
    }
}
