#include "DeviceIo.h"

#include "DeviceIoApp.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

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
        // Ensure temp buffer is large enough
        if (tempBuffer.getNumChannels() < numChannels || tempBuffer.getNumSamples() < numSamples)
            tempBuffer.setSize(numChannels, numSamples, false, false, true);

        // Read hardware input into temp buffer
        auto& toObsBuffer = deviceIoApp->getToObsBuffer();
        bool hasHardwareInput =
            toObsBuffer.read(tempBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate, false);

        // Prepare output buffer for hardware (with delay applied)
        juce::AudioBuffer<float> hardwareOutputBuffer;

        // Send to hardware output based on mode
        auto& fromObsBuffer = deviceIoApp->getFromObsBuffer();
        if (hasHardwareInput)
        {
            if (this->mixInput)
            {
                // When both HW input and output are selected with mix ON:
                // Send OBS input + hardware input to hardware output
                hardwareOutputBuffer.setSize(numChannels, numSamples, false, false, true);
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* mixDest = hardwareOutputBuffer.getWritePointer(ch);
                    const auto* obsInput = buffer[ch];
                    const auto* hwInput = tempBuffer.getReadPointer(ch);
                    for (int i = 0; i < numSamples; ++i)
                        mixDest[i] = obsInput[i] + hwInput[i];
                }
                // Apply output delay before sending to hardware
                applyOutputDelay(hardwareOutputBuffer, numChannels, numSamples, sampleRate);
                fromObsBuffer
                    .write(hardwareOutputBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate);
            }
            else
            {
                // When both HW input and output are selected with mix OFF:
                // Send only hardware input to hardware output
                hardwareOutputBuffer.setSize(numChannels, numSamples, false, false, true);
                for (int ch = 0; ch < numChannels; ++ch)
                    hardwareOutputBuffer.copyFrom(ch, 0, tempBuffer, ch, 0, numSamples);
                // Apply output delay before sending to hardware
                applyOutputDelay(hardwareOutputBuffer, numChannels, numSamples, sampleRate);
                fromObsBuffer
                    .write(hardwareOutputBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate);
            }
        }
        else
        {
            // No hardware input: send OBS input to hardware output
            hardwareOutputBuffer.setSize(numChannels, numSamples, false, false, true);
            for (int ch = 0; ch < numChannels; ++ch)
                std::copy(buffer[ch], buffer[ch] + numSamples, hardwareOutputBuffer.getWritePointer(ch));
            // Apply output delay before sending to hardware
            applyOutputDelay(hardwareOutputBuffer, numChannels, numSamples, sampleRate);
            fromObsBuffer.write(hardwareOutputBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate);
        }

        // Send to OBS output based on mode
        if (hasHardwareInput)
        {
            // Handle mixing or replacing based on mixInput flag
            if (this->mixInput)
            {
                // Mix: OBS input + HW input -> OBS output
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
                // Replace: HW input -> OBS output
                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy(tempBuffer.getReadPointer(ch), tempBuffer.getReadPointer(ch) + numSamples, buffer[ch]);
            }
        }
        // else: No hardware input - pass through OBS audio unchanged
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

        // Add output delay to state
        state->setAttribute("outputDelayMs", outputDelayMs.load(std::memory_order_acquire));

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

        // Restore output delay
        if (element->hasAttribute("outputDelayMs"))
        {
            float delayMs = static_cast<float>(element->getDoubleAttribute("outputDelayMs"));
            outputDelayMs.store(delayMs, std::memory_order_release);
        }

        deviceManager->initialise(0, 0, element.get(), false);
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
        // Prepare delay lines if not ready or parameters changed
        if (!delayPrepared || outputDelayLines.size() != numChannels)
            prepareOutputDelay(numChannels, numSamples, sampleRate);

        // Get current delay setting
        float delayMs = outputDelayMs.load(std::memory_order_acquire);
        float delaySamples = (delayMs / 1000.0f) * static_cast<float>(sampleRate);

        // Apply delay to each channel
        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (ch < outputDelayLines.size())
            {
                // Set target delay value
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
    juce::AudioDeviceManager* deviceManager = nullptr;
    DeviceIoApp* deviceIoApp = nullptr;
    AudioAppMainWindow* mainWindow = nullptr;

    std::vector<juce::Interpolators::Lagrange> interpolators;
    juce::AudioBuffer<float> tempBuffer;

    // Output delay (applied before sending to hardware)
    std::vector<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>> outputDelayLines;
    std::vector<juce::LinearSmoothedValue<float>> outputDelaySmooth;
    std::atomic<float> outputDelayMs{0.0f};
    bool delayPrepared = false;

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
