#include "../PluginHost2.h"

#include "UI/MainHostWindow.h"
#include "VirtualAudioIoDevice.h"

struct atk::PluginHost2::Impl : public juce::Timer
{
    Impl()
        : mainHostWindow(new MainHostWindow())
    {
        mainHostWindow->setVisible(false);
    }

    ~Impl()
    {
        auto* window = this->mainHostWindow.release();
        auto lambda = [window] { delete window; };
        juce::MessageManager::callAsync(lambda);
#ifdef JUCE_DEBUG
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // give time for the async call to complete
#endif
    }

    void timerCallback() override
    {
    }

    void initialise(int numInputChannels, int numOutputChannels, double sampleRate, void* obs_parent_source)
    {
        auto& dm = mainHostWindow->getDeviceManager();
        dm.addAudioDeviceType(std::make_unique<VirtualAudioIODeviceType>());
        dm.setCurrentAudioDeviceType(IO_TYPE, true);
        juce::AudioDeviceManager::AudioDeviceSetup setup = dm.getAudioDeviceSetup();
        setup.inputDeviceName = IO_NAME;
        setup.outputDeviceName = IO_NAME;
        auto res = dm.setAudioDeviceSetup(setup, true);
        virtualAudioIODevice = dynamic_cast<VirtualAudioIODevice*>(dm.getCurrentAudioDevice());
    }

    void process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
    {
        virtualAudioIODevice->process(buffer, newNumChannels, newNumSamples);
    }

    void setVisible(bool visible)
    {
        if (!mainHostWindow->isOnDesktop() && visible)
        {
            mainHostWindow->addToDesktop();
            mainHostWindow->toFront(true);
        }
        mainHostWindow->setVisible(visible);
    }

    void getState(std::string& s)
    {
        juce::XmlElement xml("atkAudioPluginHost2State");

        auto state = mainHostWindow->getDeviceManager().createStateXml();
        if (state != nullptr)
            xml.addChildElement(state.release());

        juce::XmlElement* filterGraph;
        filterGraph = new juce::XmlElement("FILTERGRAPH");

        mainHostWindow->getGraphXml(*filterGraph);
        xml.addChildElement(filterGraph);

        s = xml.toString().toStdString();
    }

    void setState(std::string& s)
    {
        if (s.empty())
            return;

        juce::String xmlString(s);
        juce::MessageManager::callAsync(
            [this, xmlString]()
            {
                auto xml = juce::XmlDocument::parse(xmlString);

                juce::XmlElement* filterGraph = xml->getChildByName("FILTERGRAPH");
                if (filterGraph != nullptr)
                    this->mainHostWindow->setGraphXml(*filterGraph);

                auto* savedState = xml->getChildByName("DEVICESETUP");
                auto& deviceManager = this->mainHostWindow->getDeviceManager();
                if (savedState)
                    deviceManager.initialise(256, 256, savedState, true);
            }
        );
    }

private:
    std::unique_ptr<MainHostWindow> mainHostWindow;
    std::unique_ptr<juce::AudioDeviceManager> deviceManager;

    VirtualAudioIODevice* virtualAudioIODevice = nullptr;
};

void atk::PluginHost2::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::PluginHost2::setVisible(bool visible)
{
    pImpl->setVisible(visible);
}

void atk::PluginHost2::getState(std::string& s)
{
    pImpl->getState(s);
}

void atk::PluginHost2::setState(std::string& s)
{
    pImpl->setState(s);
}

void atk::PluginHost2::initialise(
    int numInputChannels,
    int numOutputChannels,
    double sampleRate,
    void* obs_parent_source
)
{
    pImpl->initialise(numInputChannels, numOutputChannels, sampleRate, obs_parent_source);
}

atk::PluginHost2::PluginHost2()
    : pImpl(new Impl())
{
}

atk::PluginHost2::~PluginHost2()
{
    delete pImpl;
}
