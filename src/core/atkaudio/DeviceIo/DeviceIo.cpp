#include "DeviceIo.h"

#include "../atkAudioModule.h"
#include "DeviceIoApp.h"

#include <algorithm>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#define MAX_CHANNELS 256

atk::DeviceIo::DeviceIo()
    : deviceIoApp(std::make_unique<DeviceIoApp>(MAX_CHANNELS, MAX_CHANNELS))
    , mainWindow(std::make_unique<AudioAppMainWindow>(*deviceIoApp))
{
}

atk::DeviceIo::~DeviceIo()
{
    auto* window = mainWindow.release();
    auto* app = deviceIoApp.release();

    atk::atkAudioModule::destroyOnMessageThread(
        [window, app]()
        {
            if (window != nullptr)
            {
                window->setVisible(false);
                if (window->isOnDesktop())
                    window->removeFromDesktop();
            }

            if (app != nullptr)
                app->getDeviceManager().closeAudioDevice();

            delete window;
            delete app;
        }
    );
}

void atk::DeviceIo::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    if (deviceIoApp == nullptr || buffer == nullptr)
        return;

    bool currentBypass = bypass.load(std::memory_order_acquire);
    float targetGain = currentBypass ? 0.0f : 1.0f;

    if (fadeGain.getTargetValue() != targetGain)
    {
        fadeGain.reset(sampleRate, fadeDurationSeconds.load(std::memory_order_acquire));

        if (!currentBypass)
        {
            auto& toObsBuffer = deviceIoApp->getToObsBuffer();
            auto& fromObsBuffer = deviceIoApp->getFromObsBuffer();
            toObsBuffer.reset();
            fromObsBuffer.reset();
        }

        fadeGain.setTargetValue(targetGain);
    }

    if (currentBypass && !fadeGain.isSmoothing())
        return;

    if (fadeGain.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float gain = fadeGain.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer[ch][i] *= gain;
        }
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
        hardwareOutputBuffer.setSize(numChannels, numSamples, false, false, true);

        if (mixInput)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* mixDest = hardwareOutputBuffer.getWritePointer(ch);
                const auto* obsInput = buffer[ch];
                const auto* hwInput = tempBuffer.getReadPointer(ch);
                for (int i = 0; i < numSamples; ++i)
                    mixDest[i] = obsInput[i] + hwInput[i];
            }
        }
        else
        {
            for (int ch = 0; ch < numChannels; ++ch)
                hardwareOutputBuffer.copyFrom(ch, 0, tempBuffer, ch, 0, numSamples);
        }

        applyOutputDelay(hardwareOutputBuffer, numChannels, numSamples, sampleRate);
        fromObsBuffer.write(hardwareOutputBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate);
    }
    else
    {
        hardwareOutputBuffer.setSize(numChannels, numSamples, false, false, true);
        for (int ch = 0; ch < numChannels; ++ch)
            std::copy(buffer[ch], buffer[ch] + numSamples, hardwareOutputBuffer.getWritePointer(ch));

        applyOutputDelay(hardwareOutputBuffer, numChannels, numSamples, sampleRate);
        fromObsBuffer.write(hardwareOutputBuffer.getArrayOfWritePointers(), numChannels, numSamples, sampleRate);
    }

    if (!hasHardwareInput)
        return;

    if (mixInput)
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

void atk::DeviceIo::setBypass(bool shouldBypass)
{
    bypass.store(shouldBypass, std::memory_order_release);
}

bool atk::DeviceIo::isBypassed() const
{
    return bypass.load(std::memory_order_acquire);
}

void atk::DeviceIo::setFadeTime(double seconds)
{
    fadeDurationSeconds.store(seconds, std::memory_order_release);
}

void atk::DeviceIo::setMixInput(bool shouldMixInput)
{
    mixInput = shouldMixInput;
}

void atk::DeviceIo::setOutputDelay(float delayMs)
{
    outputDelayMs.store(delayMs, std::memory_order_release);
}

float atk::DeviceIo::getOutputDelay() const
{
    return outputDelayMs.load(std::memory_order_acquire);
}

void atk::DeviceIo::getState(std::string& s)
{
    if (deviceIoApp == nullptr)
    {
        s.clear();
        return;
    }

    juce::XmlElement state("DEVICEIO_STATE");
    state.setAttribute("outputDelayMs", outputDelayMs.load(std::memory_order_acquire));

    auto& dm = deviceIoApp->getDeviceManager();
    auto setup = dm.getAudioDeviceSetup();

    auto* deviceSetup = new juce::XmlElement("DEVICESETUP");
    deviceSetup->setAttribute("deviceType", dm.getCurrentAudioDeviceType());
    deviceSetup->setAttribute("audioOutputDeviceName", setup.outputDeviceName);
    deviceSetup->setAttribute("audioInputDeviceName", setup.inputDeviceName);
    deviceSetup->setAttribute("audioDeviceRate", setup.sampleRate);
    deviceSetup->setAttribute("audioDeviceBufferSize", setup.bufferSize);
    deviceSetup->setAttribute("audioDeviceInChans", setup.inputChannels.toString(2));
    deviceSetup->setAttribute("audioDeviceOutChans", setup.outputChannels.toString(2));
    state.addChildElement(deviceSetup);

    s = state.toString().toStdString();
}

void atk::DeviceIo::setState(std::string& s)
{
    if (deviceIoApp == nullptr || s.empty())
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

    if (auto* deviceState = element->getChildByName("DEVICESETUP"))
    {
        juce::XmlElement wrapper("AUDIODEVICEMANAGERSTATE");
        wrapper.addChildElement(new juce::XmlElement(*deviceState));
        deviceIoApp->setStateXml(wrapper.toString());
    }
}

juce::Component* atk::DeviceIo::getWindowComponent()
{
    return mainWindow.get();
}

void atk::DeviceIo::applyOutputDelay(
    juce::AudioBuffer<float>& buffer,
    int numChannels,
    int numSamples,
    double sampleRate
)
{
    if (!delayPrepared || static_cast<int>(outputDelayLines.size()) != numChannels)
        prepareOutputDelay(numChannels, numSamples, sampleRate);

    float delayMs = outputDelayMs.load(std::memory_order_acquire);
    float delaySamples = (delayMs / 1000.0f) * static_cast<float>(sampleRate);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        if (ch >= static_cast<int>(outputDelayLines.size()))
            continue;

        outputDelaySmooth[ch].setTargetValue(delaySamples);

        auto* channelData = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
        {
            outputDelayLines[ch].pushSample(0, channelData[i]);
            channelData[i] = outputDelayLines[ch].popSample(0, outputDelaySmooth[ch].getNextValue());
        }
    }
}

void atk::DeviceIo::prepareOutputDelay(int numChannels, int numSamples, double sampleRate)
{
    outputDelayLines.clear();
    outputDelayLines.resize(numChannels);

    for (auto& delayLine : outputDelayLines)
    {
        delayLine.prepare(juce::dsp::ProcessSpec{sampleRate, static_cast<uint32_t>(numSamples), 1});
        delayLine.reset();
        delayLine.setMaximumDelayInSamples(10 * static_cast<int>(sampleRate));
        delayLine.setDelay(0.0f);
    }

    outputDelaySmooth.clear();
    outputDelaySmooth.resize(numChannels);
    for (auto& smooth : outputDelaySmooth)
        smooth.reset(sampleRate, 0.4f);

    delayPrepared = true;
}
