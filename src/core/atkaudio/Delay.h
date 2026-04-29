#pragma once

#include "atkaudio.h"

#include <atomic>
#include <juce_dsp/juce_dsp.h>
#include <vector>

namespace atk
{
class Delay : private juce::Timer
{
public:
    Delay();
    ~Delay();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate);
    void setDelay(float delay);

private:
    void timerCallback() override;
    void prepare(int newNumChannels, int newNumSamples, double newSampleRate);

    std::atomic_bool isPrepared{false};

    int numChannels = 2;
    int numSamples = 256;
    double sampleRate = 48000.0;

    std::vector<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>> delayLine;
    std::vector<juce::LinearSmoothedValue<float>> delayTimeSmooth;
};
} // namespace atk
