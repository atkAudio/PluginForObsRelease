#include "../DeviceIo.h"

#include "AudioAppDemo.h"

#include <juce_audio_utils/juce_audio_utils.h>

#define MAX_CHANNELS 256

struct atk::DeviceIo::Impl : public juce::Timer
{
    Impl()
        : deviceManager(new juce::AudioDeviceManager())
        , audioAppDemo(new AudioAppDemo(*deviceManager, MAX_CHANNELS, MAX_CHANNELS, 48000))
        , mainWindow(new AudioAppMainWindow(*audioAppDemo)) // mainWindow takes ownership
    {
    }

    ~Impl()
    {
        auto* window = this->mainWindow;
        auto* manager = this->deviceManager;
        auto lambda = [window, manager]()
        {
            delete window;
            delete manager;
        };
        juce::MessageManager::callAsync(lambda);
    }

    void timerCallback() override
    {
    }

    void process(float** buffer, int numChannels, int numSamples, double sampleRate)
    {
        audioAppDemo->setRemoteSampleRate(sampleRate);
        audioAppDemo->setRemoteBufferSize(numSamples);

        if (!audioAppDemo->getPrepareLock().tryEnter())
            return;

        if (!audioAppDemo->getIsPrepared())
        {
            audioAppDemo->getPrepareLock().exit();
            return;
        }

        tempBuffer.resize(numSamples, 0.0f);

        auto& outputFifo = audioAppDemo->getOutputFifo();
        auto outputChannels = outputFifo.getNumChannels();
        if (outputChannels == 1)
        {
            std::fill(tempBuffer.begin(), tempBuffer.end(), 0.0f);
            for (int i = 0; i < numChannels; i++)
                for (int j = 0; j < numSamples; j++)
                    tempBuffer[j] += buffer[i][j] * (numChannels >= 2 ? 0.5f : 1.0f);

            outputFifo.write(tempBuffer.data(), 0, numSamples, true);
        }

        if (outputChannels >= 2)
        {
            if (outputChannels > numChannels)
                outputChannels = numChannels;
            for (auto i = 0; i < outputChannels; ++i)
                outputFifo.write(buffer[i], i, numSamples, i == outputChannels - 1);
        }

        auto sampleRatio = audioAppDemo->getSampleRate() / sampleRate;
        // auto remoteBufferSize = audioAppDemo->getBufferSize();

        auto sampleRatioCorrection = 1.0;
        if (speedUp)
            sampleRatioCorrection *= 1.00111;
        if (speedDown)
            sampleRatioCorrection *= (1 / 1.00111);

        sampleRatio *= sampleRatioCorrection;

        auto numInputSamplesReady = audioAppDemo->getInputFifo().getNumReady();

        if (numInputSamplesReady / sampleRatio < numSamples)
        {
            audioAppDemo->getPrepareLock().exit();
            return;
        }

        tempBuffer.resize(numInputSamplesReady, 0.0f);

        int consumedSamples = 0;

        auto& inputFifo = audioAppDemo->getInputFifo();
        auto inputChannels = audioAppDemo->getInputFifo().getNumChannels();

        if (interpolators.size() != inputChannels)
        {
            interpolators.resize(inputChannels);
            for (auto& i : interpolators)
                i.reset();
        }

        if (!mixInput)
            for (int i = 0; i < numChannels; i++)
                for (int j = 0; j < numSamples; j++)
                    buffer[i][j] = 0.0f;

        for (auto i = 0; i < inputChannels; ++i)
        {
            inputFifo.read(tempBuffer.data(), i, numInputSamplesReady, false);

            auto targetChannel = i % numChannels;

            auto gain = 1.0f;
            if (numChannels == 1)
                gain = 0.5f;

            consumedSamples = interpolators[i].processAdding(
                sampleRatio,
                tempBuffer.data(),
                buffer[targetChannel],
                numSamples,
                numInputSamplesReady,
                0,
                gain
            );
        }

        if (consumedSamples > inputFifo.getNumReady())
            consumedSamples = inputFifo.getNumReady();

        inputFifo.advanceRead(consumedSamples);

        sampleRatio = audioAppDemo->getRemoteSampleRate() / sampleRate;

        auto minSamples = std::min(numSamples, (int)(audioAppDemo->getBufferSize() / sampleRatio));
        auto maxSamples = std::max(numSamples, (int)(audioAppDemo->getBufferSize() / sampleRatio));

        maxSamples = maxSamples * 2;
        if (speedUp)
            maxSamples /= 2;

        if (speedDown)
            minSamples *= 2;

        numInputSamplesReady = (int)(inputFifo.getNumReady() / sampleRatio);

        if (numInputSamplesReady < minSamples)
            speedDown = true;
        else if (speedDown)
        {
            speedDown = false;
#if JUCE_DEBUG
            auto timeAndDate = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S");
            DBG("input speed down " << timeAndDate);
#endif
        }

        if (numInputSamplesReady > maxSamples)
            speedUp = true;
        else if (speedUp)
        {
            speedUp = false;
#if JUCE_DEBUG
            auto timeAndDate = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S");
            DBG("input speed up " << timeAndDate);
#endif
        }

        audioAppDemo->getPrepareLock().exit();
    }

    void setVisible(bool visible)
    {
        mainWindow->setVisible(visible);
        if (visible && mainWindow->isMinimised())
            mainWindow->setMinimised(false);
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

        auto capacity = s.capacity();
        auto stateStringSize = stateString.size();
        if (stateStringSize > capacity)
            return;

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
    AudioAppDemo* audioAppDemo = nullptr;
    AudioAppMainWindow* mainWindow = nullptr;

    std::vector<juce::Interpolators::Lagrange> interpolators;

    bool mixInput = false;
    bool speedUp = false;
    bool speedDown = false;

    std::vector<float> tempBuffer;
};

void atk::DeviceIo::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::DeviceIo::setVisible(bool visible)
{
    pImpl->setVisible(visible);
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

atk::DeviceIo::DeviceIo()
    : pImpl(new Impl())
{
}

atk::DeviceIo::~DeviceIo()
{
    delete pImpl;
}
