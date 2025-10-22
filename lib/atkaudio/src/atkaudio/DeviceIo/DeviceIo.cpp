#include "DeviceIo.h"

#include "DeviceIoApp.h"

#include <juce_audio_utils/juce_audio_utils.h>

#define MAX_CHANNELS 256

struct atk::DeviceIo::Impl : public juce::Timer
{
    Impl()
        : deviceManager(new juce::AudioDeviceManager())
        , deviceIoApp(new DeviceIoApp(*deviceManager, MAX_CHANNELS, MAX_CHANNELS, 48000))
        , mainWindow(new AudioAppMainWindow(*deviceIoApp)) // mainWindow takes ownership
    {
    }

    ~Impl()
    {
        // Clean up asynchronously on the message thread
        auto* window = this->mainWindow;
        auto* manager = this->deviceManager;
        auto lambda = [window, manager]
        {
            delete window;
            delete manager;
        };
        juce::MessageManager::callAsync(lambda);
    }

    void timerCallback() override
    {
    }

    // processBlock
    void process(float** buffer, int numChannels, int numSamples, double sampleRate)
    {
        auto& fromObsBuffer = deviceIoApp->getFromObsBuffer();
        fromObsBuffer.write(buffer, numChannels, numSamples, sampleRate);

        auto& toObsBuffer = deviceIoApp->getToObsBuffer();
        toObsBuffer.read(buffer, numChannels, numSamples, sampleRate, this->mixInput);
    }

    juce::Component* getWindowComponent()
    {
        return mainWindow;
    }

    void getState(std::string& s)
    {
        auto state = deviceManager->createStateXml();

        if (!state)
        {
            s = " ";
            return;
        }

        auto stateString = state->toString().toStdString();

        s = stateString;
    }

    void setState(std::string& s)
    {
        if (s.empty())
            return;
        juce::XmlDocument chunkDataXml(s);
        auto element = chunkDataXml.getDocumentElement();
        if (!element)
            return;

        deviceManager->initialise(0, 0, element.get(), false);
    }

    void setMixInput(bool val)
    {
        this->mixInput = val;
    }

private:
    juce::AudioDeviceManager* deviceManager = nullptr;
    DeviceIoApp* deviceIoApp = nullptr;
    AudioAppMainWindow* mainWindow = nullptr;

    std::vector<juce::Interpolators::Lagrange> interpolators;

    bool mixInput = false;
};

void atk::DeviceIo::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::DeviceIo::setMixInput(bool mixInput)
{
    pImpl->setMixInput(mixInput);
}

void atk::DeviceIo::getState(std::string& s)
{
    pImpl->getState(s);
}

void atk::DeviceIo::setState(std::string& s)
{
    pImpl->setState(s);
}

juce::Component* atk::DeviceIo::getWindowComponent()
{
    return pImpl->getWindowComponent();
}

atk::DeviceIo::DeviceIo()
    : pImpl(new Impl())
{
}

atk::DeviceIo::~DeviceIo()
{
    delete pImpl;
}
