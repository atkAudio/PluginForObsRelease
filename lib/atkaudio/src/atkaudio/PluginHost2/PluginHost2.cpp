#include "API/PluginHost2.h"

#include "UI/MainHostWindow.h"
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleBridge.h>

struct atk::PluginHost2::Impl : public juce::AsyncUpdater
{
    // Member variables
    std::unique_ptr<MainHostWindow> mainHostWindow;
    std::unique_ptr<atk::ModuleDeviceManager> moduleDeviceManager;

    // State restoration data (accessed from message thread only)
    juce::String pendingStateString;

    Impl()
        : mainHostWindow(new MainHostWindow())
    {
        // Create ModuleDeviceManager for audio/MIDI infrastructure
        moduleDeviceManager = std::make_unique<atk::ModuleDeviceManager>(
            std::make_unique<atk::ModuleAudioIODeviceType>("PluginHost2 Audio"),
            mainHostWindow->getDeviceManager()
            // ModuleDeviceManager creates its own internal MidiClient by default
        );

        // Initialize device management using ModuleInfrastructure
        if (moduleDeviceManager->initialize())
            moduleDeviceManager->openOBSDevice();

        // MainHostWindow uses ModuleDeviceManager's MidiClient
        mainHostWindow->setExternalMidiClient(moduleDeviceManager->getMidiClient());

        mainHostWindow->setVisible(false);
    }

    ~Impl()
    {
        // Cancel any pending async updates
        cancelPendingUpdate();

        // CRITICAL: Clean up ModuleDeviceManager FIRST before window destruction
        // It holds a reference to mainHostWindow's AudioDeviceManager
        if (moduleDeviceManager)
        {
            moduleDeviceManager->cleanup();
            moduleDeviceManager.reset();
        }

        // Clean up everything on the message thread since OBS destroys filters on background threads
        auto* window = this->mainHostWindow.release();
        auto lambda = [window]
        {
            // Delete the window
            delete window;
        };
        juce::MessageManager::callAsync(lambda);
    }

    void handleAsyncUpdate() override
    {
        // Called on message thread to restore state safely
        if (pendingStateString.isEmpty())
            return;

        if (mainHostWindow->graphHolder != nullptr && mainHostWindow->graphHolder->graph != nullptr)
        {
            // Clear graph first - this removes the default nodes
            mainHostWindow->graphHolder->graph->clear();

            // Restore from saved state
            auto xml = juce::XmlDocument::parse(pendingStateString);
            if (!xml)
            {
                pendingStateString.clear();
                return;
            }

            // CRITICAL: Restore device manager state FIRST
            // This ensures AudioServer devices are available before graph restoration
            auto* savedState = xml->getChildByName("DEVICESETUP");
            auto& deviceManager = mainHostWindow->getDeviceManager();
            if (savedState)
                deviceManager.initialise(256, 256, savedState, true);

            // Now restore filter graph - AudioServer devices are now available
            juce::XmlElement* filterGraph = xml->getChildByName("FILTERGRAPH");
            if (filterGraph != nullptr)
                mainHostWindow->setGraphXml(*filterGraph);

            // AFTER graph restoration, apply AudioServer device settings
            // Devices are now open, so we can change their buffer size and sample rate
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

                        // Restore sample rate if saved
                        if (deviceElement->hasAttribute("sampleRate"))
                        {
                            double sampleRate = deviceElement->getDoubleAttribute("sampleRate");
                            DBG("PluginHost2: Restoring sample rate for "
                                + deviceName
                                + " to "
                                + juce::String(sampleRate));
                            audioServer->setDeviceSampleRate(deviceName, sampleRate);
                        }

                        // Restore buffer size if saved
                        if (deviceElement->hasAttribute("bufferSize"))
                        {
                            int bufferSize = deviceElement->getIntAttribute("bufferSize");
                            DBG("PluginHost2: Restoring buffer size for "
                                + deviceName
                                + " to "
                                + juce::String(bufferSize));
                            audioServer->setDeviceBufferSize(deviceName, bufferSize);
                        }
                    }
                }
            }

            // Restore MIDI client subscriptions
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

    juce::Component* getWindowComponent()
    {
        return mainHostWindow.get();
    }

    void getState(std::string& s)
    {
        juce::XmlElement xml("atkAudioPluginHost2State");

        // Save device manager state
        auto state = mainHostWindow->getDeviceManager().createStateXml();
        if (state != nullptr)
            xml.addChildElement(state.release());

        // Save AudioServer device settings (buffer size, sample rate for each open device)
        auto* audioServerElement = new juce::XmlElement("AUDIOSERVER");
        if (auto* audioServer = atk::AudioServer::getInstance())
        {
            // Get all input and output device names
            auto inputDevices = audioServer->getAvailableInputDevices();
            auto outputDevices = audioServer->getAvailableOutputDevices();

            // Combine and deduplicate device names
            juce::StringArray allDevices;
            allDevices.addArray(inputDevices);
            for (const auto& dev : outputDevices)
                if (!allDevices.contains(dev))
                    allDevices.add(dev);

            // Save settings for each device that is currently open
            for (const auto& deviceName : allDevices)
            {
                double sampleRate = audioServer->getCurrentSampleRate(deviceName);
                int bufferSize = audioServer->getCurrentBufferSize(deviceName);

                // Only save if device is open (has valid settings)
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

        // Save filter graph
        juce::XmlElement* filterGraph;
        filterGraph = new juce::XmlElement("FILTERGRAPH");

        mainHostWindow->getGraphXml(*filterGraph);
        xml.addChildElement(filterGraph);

        // Save MIDI client subscriptions
        auto midiState = mainHostWindow->getMidiClient().getSubscriptions();
        auto* midiElement = new juce::XmlElement("MIDISTATE");
        midiElement->setAttribute("state", midiState.serialize());
        xml.addChildElement(midiElement);

        s = xml.toString().toStdString();
    }

    void setState(std::string& s)
    {
        // If state is empty, keep the default graph created by newDocument()
        if (s.empty())
            return;

        // Store state string and trigger async restoration on message thread
        // This avoids dangling 'this' pointer if the module is destroyed
        pendingStateString = juce::String(s);
        triggerAsyncUpdate();
    }

    void process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
    {
        // Use ModuleDeviceManager for realtime-safe external audio processing
        if (moduleDeviceManager)
            moduleDeviceManager->processExternalAudio(buffer, newNumChannels, newNumSamples, newSampleRate);
    }
};

void atk::PluginHost2::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::PluginHost2::getState(std::string& s)
{
    pImpl->getState(s);
}

void atk::PluginHost2::setState(std::string& s)
{
    pImpl->setState(s);
}

juce::Component* atk::PluginHost2::getWindowComponent()
{
    return pImpl->getWindowComponent();
}

atk::PluginHost2::PluginHost2()
    : pImpl(new Impl())
{
}

atk::PluginHost2::~PluginHost2()
{
    delete pImpl;
}
