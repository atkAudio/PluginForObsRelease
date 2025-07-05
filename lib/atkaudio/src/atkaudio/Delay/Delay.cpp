#include "../Delay.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

struct atk::Delay::Impl : public juce::Timer
{
    std::atomic_bool isPrepared{false};

    int numChannels = 2;
    int numSamples = 256;
    double sampleRate = 48000.0;

    std::vector<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>> delayLine;
    std::vector<juce::LinearSmoothedValue<float>> delayTimeSmooth;

    Impl()
    {
        startTimerHz(30);
    }

    ~Impl()
    {
        stopTimer();
    }

    void timerCallback() override
    {
        if (!isPrepared.load(std::memory_order_acquire))
        {
            prepare(this->numChannels, this->numSamples, this->sampleRate);
            isPrepared.store(true, std::memory_order_release);
        }
    }

    void prepare(int newNumChannels, int newNumSamples, double newSampleRate)
    {
        delayLine.clear();
        delayLine.resize(newNumChannels);
        for (auto& i : delayLine)
        {
            i.prepare(juce::dsp::ProcessSpec({newSampleRate, (uint32_t)newNumSamples, (uint32_t)1}));
            i.reset();
            i.setMaximumDelayInSamples(10 * (int)newSampleRate);
            i.setDelay(0.0f);
        }

        delayTimeSmooth.clear();
        delayTimeSmooth.resize(newNumChannels);
        for (auto& i : delayTimeSmooth)
            i.reset(newSampleRate, 0.4f);

        isPrepared.store(true, std::memory_order_release);
    }

    void process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
    {
        if (this->numChannels != newNumChannels || this->numSamples != newNumSamples
            || this->sampleRate != newSampleRate)
        {
            this->numChannels = newNumChannels;
            this->numSamples = newNumSamples;
            this->sampleRate = newSampleRate;

            isPrepared.store(false, std::memory_order_release);

            return;
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return;

        for (int i = 0; i < newNumChannels; ++i)
        {
            for (int j = 0; j < newNumSamples; ++j)
            {
                delayLine[i].pushSample(0, buffer[i][j]);
                buffer[i][j] = delayLine[i].popSample(0, delayTimeSmooth[i].getNextValue());
            }
        }
    }

    void setDelay(float delay)
    {
        for (auto& i : delayTimeSmooth)
            i.setTargetValue(delay / 1000.0f * (float)sampleRate);
    }
};

void atk::Delay::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::Delay::setDelay(float delay)
{
    pImpl->setDelay(delay);
}

atk::Delay::Delay()
    : pImpl(new Impl())
{
}

atk::Delay::~Delay()
{
    delete pImpl;
}
