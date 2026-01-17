#include "DeviceIo.h"

#include "DeviceIoApp.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#define MAX_CHANNELS 256

struct atk::DeviceIo::Impl
{
    Impl()
        : deviceIoApp(new DeviceIoApp(MAX_CHANNELS, MAX_CHANNELS))
        , mainWindow(new AudioAppMainWindow(*deviceIoApp))
    {
    }

    ~Impl()
    {
        auto* window = this->mainWindow;
        auto* app = this->deviceIoApp;
        auto lambda = [window, app]
        {
            delete window;
            delete app;
        };
        juce::MessageManager::callAsync(lambda);
    }

    void process(float** buffer, int numChannels, int numSamples, double sampleRate)
    {
        bool currentBypass = bypass.load(std::memory_order_acquire);
        bool wasJustBypassed = wasBypassed.exchange(currentBypass, std::memory_order_acq_rel);

        if (currentBypass)
            return;

        // Clear stale data from buffers when transitioning from bypassed to active
        if (wasJustBypassed)
        {
            auto& toObsBuffer = deviceIoApp->getToObsBuffer();
            auto& fromObsBuffer = deviceIoApp->getFromObsBuffer();
            toObsBuffer.reset();
            fromObsBuffer.reset();
            // Start fade-in over one buffer
            fadeInSamplesRemaining = numSamples;
            fadeInTotalSamples = numSamples;
        }

        // Apply fade-in ramp if active
        if (fadeInSamplesRemaining > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* channelData = buffer[ch];
                int fadeStart = fadeInTotalSamples - fadeInSamplesRemaining;
                for (int i = 0; i < numSamples && fadeInSamplesRemaining > 0; ++i)
                {
                    float gain = static_cast<float>(fadeStart + i + 1) / static_cast<float>(fadeInTotalSamples);
                    channelData[i] *= gain;
                }
            }
            fadeInSamplesRemaining = std::max(0, fadeInSamplesRemaining - numSamples);
        }

        if (tempBuffer.getNumChannels() < numChannels || tempBuffer.getNumSamples() < numSamples)
            tempBuffer.setSize(numChannels, numSamples, false, false, true);

        auto& toObsBuffer = deviceIoApp->getToObsBuffer();
        bool hasHardwareInput =
            toObsBuffer.read(tempBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate, false);

        juce::AudioBuffer<float> hardwareOutputBuffer;
        auto& fromObsBuffer = deviceIoApp->getFromObsBuffer();
        if (hasHardwareInput)
        {
            if (this->mixInput)
            {
                hardwareOutputBuffer.setSize(numChannels, numSamples, false, false, true);
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* mixDest = hardwareOutputBuffer.getWritePointer(ch);
                    const auto* obsInput = buffer[ch];
                    const auto* hwInput = tempBuffer.getReadPointer(ch);
                    for (int i = 0; i < numSamples; ++i)
                        mixDest[i] = obsInput[i] + hwInput[i];
                }
                applyOutputDelay(hardwareOutputBuffer, numChannels, numSamples, sampleRate);
                fromObsBuffer
                    .write(hardwareOutputBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate);
            }
            else
            {
                hardwareOutputBuffer.setSize(numChannels, numSamples, false, false, true);
                for (int ch = 0; ch < numChannels; ++ch)
                    hardwareOutputBuffer.copyFrom(ch, 0, tempBuffer, ch, 0, numSamples);
                applyOutputDelay(hardwareOutputBuffer, numChannels, numSamples, sampleRate);
                fromObsBuffer
                    .write(hardwareOutputBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate);
            }
        }
        else
        {
            hardwareOutputBuffer.setSize(numChannels, numSamples, false, false, true);
            for (int ch = 0; ch < numChannels; ++ch)
                std::copy(buffer[ch], buffer[ch] + numSamples, hardwareOutputBuffer.getWritePointer(ch));
            applyOutputDelay(hardwareOutputBuffer, numChannels, numSamples, sampleRate);
            fromObsBuffer.write(hardwareOutputBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate);
        }

        if (hasHardwareInput)
        {
            if (this->mixInput)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* dest = buffer[ch];
                    const auto* hwInput = tempBuffer.getReadPointer(ch);
                    for (int i = 0; i < numSamples; ++i)
                        dest[i] += hwInput[i];
                }
            }
            else
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy(tempBuffer.getReadPointer(ch), tempBuffer.getReadPointer(ch) + numSamples, buffer[ch]);
            }
        }
    }

    juce::Component* getWindowComponent()
    {
        return mainWindow;
    }

    void getState(std::string& s)
    {
        juce::XmlElement state("DEVICEIO_STATE");
        state.setAttribute("outputDelayMs", outputDelayMs.load(std::memory_order_acquire));

        auto deviceState = deviceIoApp->getDeviceManager().createStateXml();
        if (deviceState)
            state.addChildElement(new juce::XmlElement(*deviceState));

        s = state.toString().toStdString();
    }

    void setState(std::string& s)
    {
        if (s.empty())
            return;

        juce::XmlDocument doc(s);
        auto element = doc.getDocumentElement();
        if (!element)
            return;

        if (element->hasAttribute("outputDelayMs"))
        {
            float delayMs = static_cast<float>(element->getDoubleAttribute("outputDelayMs"));
            outputDelayMs.store(delayMs, std::memory_order_release);
        }

        auto* deviceState = element->getChildByName("DEVICESETUP");
        if (deviceState)
        {
            juce::XmlElement wrapper("AUDIODEVICEMANAGERSTATE");
            wrapper.addChildElement(new juce::XmlElement(*deviceState));
            deviceIoApp->setStateXml(wrapper.toString());
        }
    }

    void setMixInput(bool val)
    {
        this->mixInput = val;
    }

    void setOutputDelay(float delayMs)
    {
        outputDelayMs.store(delayMs, std::memory_order_release);
    }

    float getOutputDelay() const
    {
        return outputDelayMs.load(std::memory_order_acquire);
    }

    void applyOutputDelay(juce::AudioBuffer<float>& buffer, int numChannels, int numSamples, double sampleRate)
    {
        if (!delayPrepared || outputDelayLines.size() != numChannels)
            prepareOutputDelay(numChannels, numSamples, sampleRate);

        float delayMs = outputDelayMs.load(std::memory_order_acquire);
        float delaySamples = (delayMs / 1000.0f) * static_cast<float>(sampleRate);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (ch < outputDelayLines.size())
            {
                outputDelaySmooth[ch].setTargetValue(delaySamples);

                auto* channelData = buffer.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    outputDelayLines[ch].pushSample(0, channelData[i]);
                    channelData[i] = outputDelayLines[ch].popSample(0, outputDelaySmooth[ch].getNextValue());
                }
            }
        }
    }

    void prepareOutputDelay(int numChannels, int numSamples, double sampleRate)
    {
        outputDelayLines.clear();
        outputDelayLines.resize(numChannels);

        for (auto& delayLine : outputDelayLines)
        {
            delayLine.prepare(juce::dsp::ProcessSpec{sampleRate, static_cast<uint32_t>(numSamples), 1});
            delayLine.reset();
            delayLine.setMaximumDelayInSamples(10 * static_cast<int>(sampleRate)); // 10 seconds max
            delayLine.setDelay(0.0f);
        }

        outputDelaySmooth.clear();
        outputDelaySmooth.resize(numChannels);
        for (auto& smooth : outputDelaySmooth)
            smooth.reset(sampleRate, 0.4f); // 400ms smoothing time

        delayPrepared = true;
    }

private:
    DeviceIoApp* deviceIoApp = nullptr;
    AudioAppMainWindow* mainWindow = nullptr;

    juce::AudioBuffer<float> tempBuffer;
    std::vector<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>> outputDelayLines;
    std::vector<juce::LinearSmoothedValue<float>> outputDelaySmooth;
    std::atomic<float> outputDelayMs{0.0f};
    bool delayPrepared = false;

    bool mixInput = false;
    std::atomic<bool> bypass{false};
    std::atomic<bool> wasBypassed{false};
    int fadeInSamplesRemaining{0};
    int fadeInTotalSamples{0};

public:
    void setBypass(bool v)
    {
        bypass.store(v, std::memory_order_release);
    }

    bool isBypassed() const
    {
        return bypass.load(std::memory_order_acquire);
    }
};

void atk::DeviceIo::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::DeviceIo::setBypass(bool shouldBypass)
{
    if (pImpl)
        pImpl->setBypass(shouldBypass);
}

bool atk::DeviceIo::isBypassed() const
{
    return pImpl ? pImpl->isBypassed() : false;
}

void atk::DeviceIo::setMixInput(bool mixInput)
{
    pImpl->setMixInput(mixInput);
}

void atk::DeviceIo::setOutputDelay(float delayMs)
{
    pImpl->setOutputDelay(delayMs);
}

float atk::DeviceIo::getOutputDelay() const
{
    return pImpl->getOutputDelay();
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
