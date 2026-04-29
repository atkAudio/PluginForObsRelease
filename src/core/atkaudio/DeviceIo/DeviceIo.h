#pragma once

#include "../atkAudioModule.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <juce_dsp/juce_dsp.h>

class DeviceIoApp;
class AudioAppMainWindow;

namespace atk
{
class DeviceIo : public atkAudioModule
{
public:
    DeviceIo();
    ~DeviceIo();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate) override;

    // Bypass processing when filter should be inactive (e.g., not in scene)
    void setBypass(bool shouldBypass);
    bool isBypassed() const;

    // Set the fade time for bypass transitions (in seconds)
    void setFadeTime(double seconds);

    void setMixInput(bool mixInput);
    void setOutputDelay(float delayMs);
    float getOutputDelay() const;

    void getState(std::string& s) override;
    void setState(std::string& s) override;

protected:
    // AudioModule interface - only need to provide the window component
    juce::Component* getWindowComponent() override;

private:
    void prepareOutputDelay(int numChannels, int numSamples, double sampleRate);
    void applyOutputDelay(juce::AudioBuffer<float>& buffer, int numChannels, int numSamples, double sampleRate);

    std::unique_ptr<DeviceIoApp> deviceIoApp;
    std::unique_ptr<AudioAppMainWindow> mainWindow;

    juce::AudioBuffer<float> tempBuffer;
    std::vector<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>> outputDelayLines;
    std::vector<juce::LinearSmoothedValue<float>> outputDelaySmooth;
    std::atomic<float> outputDelayMs{0.0f};
    bool delayPrepared = false;

    bool mixInput = false;
    std::atomic<bool> bypass{false};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> fadeGain{1.0f};
    std::atomic<double> fadeDurationSeconds{0.5};
};
} // namespace atk