#include "../Delay.h"

void atk::Delay::timerCallback()
{
    if (!isPrepared.load(std::memory_order_acquire))
    {
        prepare(numChannels, numSamples, sampleRate);
        isPrepared.store(true, std::memory_order_release);
    }
}

void atk::Delay::prepare(int newNumChannels, int newNumSamples, double newSampleRate)
{
    delayLine.clear();
    delayLine.resize(newNumChannels);
    for (auto& channelDelayLine : delayLine)
    {
        channelDelayLine.prepare(juce::dsp::ProcessSpec({newSampleRate, (uint32_t)newNumSamples, (uint32_t)1}));
        channelDelayLine.reset();
        channelDelayLine.setMaximumDelayInSamples(10 * (int)newSampleRate);
        channelDelayLine.setDelay(0.0f);
    }

    delayTimeSmooth.clear();
    delayTimeSmooth.resize(newNumChannels);
    for (auto& smoothedDelay : delayTimeSmooth)
        smoothedDelay.reset(newSampleRate, 0.4f);

    isPrepared.store(true, std::memory_order_release);
}

void atk::Delay::process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
{
    if (numChannels != newNumChannels || numSamples != newNumSamples || sampleRate != newSampleRate)
    {
        numChannels = newNumChannels;
        numSamples = newNumSamples;
        sampleRate = newSampleRate;

        isPrepared.store(false, std::memory_order_release);
        return;
    }

    if (!isPrepared.load(std::memory_order_acquire))
        return;

    for (int channel = 0; channel < newNumChannels; ++channel)
    {
        for (int sample = 0; sample < newNumSamples; ++sample)
        {
            delayLine[channel].pushSample(0, buffer[channel][sample]);
            buffer[channel][sample] = delayLine[channel].popSample(0, delayTimeSmooth[channel].getNextValue());
        }
    }
}

void atk::Delay::setDelay(float delay)
{
    for (auto& smoothedDelay : delayTimeSmooth)
        smoothedDelay.setTargetValue(delay / 1000.0f * (float)sampleRate);
}

atk::Delay::Delay()
{
    startTimerHz(30);
}

atk::Delay::~Delay()
{
    stopTimer();
}
