#include "PluginHost2.h"

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
    }

    void timerCallback() override
    {
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

    void process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
    {
        (void)newSampleRate;
        outputData.setSize(newNumChannels, newNumSamples, false, false, true);

        auto& dm = mainHostWindow->getDeviceManager();

        auto* currentDevice = dm.getCurrentAudioDevice();
        if (!currentDevice || currentDevice->getTypeName() != IO_TYPE)
        {
            for (int ch = 0; ch < newNumChannels; ++ch)
                std::fill(buffer[ch], buffer[ch] + newNumSamples, 0.0f);

            return;
        }

        juce::ScopedLock lock(dm.getAudioCallbackLock());
        hostTimeNs = juce::Time::getHighResolutionTicks();

        currentDevice = dm.getCurrentAudioDevice();
        auto* vdev = static_cast<VirtualAudioIODevice*>(currentDevice);
        auto* cb = vdev->getAudioDeviceCallback();
        if (cb && vdev && currentDevice && currentDevice->getTypeName() == IO_TYPE)
        {
            cb->audioDeviceIOCallbackWithContext(
                const_cast<const float**>(buffer),
                newNumChannels,
                outputData.getArrayOfWritePointers(),
                newNumChannels,
                newNumSamples,
                context
            );
            for (int ch = 0; ch < newNumChannels; ++ch)
                std::memcpy(buffer[ch], outputData.getReadPointer(ch), newNumSamples * sizeof(float));
        }
    }

private:
    std::unique_ptr<MainHostWindow> mainHostWindow;
    std::unique_ptr<juce::AudioDeviceManager> deviceManager;

    juce::AudioBuffer<float> outputData{MAX_AUDIO_CHANNELS, AUDIO_OUTPUT_FRAMES};
    uint64_t hostTimeNs;
    juce::AudioIODeviceCallbackContext context{.hostTimeNs = &hostTimeNs};
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

atk::PluginHost2::PluginHost2()
    : pImpl(new Impl())
{
}

atk::PluginHost2::~PluginHost2()
{
    delete pImpl;
}
