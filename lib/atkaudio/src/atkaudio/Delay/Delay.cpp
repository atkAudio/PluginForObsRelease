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

    void prepare(int numChannels, int numSamples, double sampleRate)
    {
        delayLine.clear();
        delayLine.resize(numChannels);
        for (auto& i : delayLine)
        {
            i.prepare(juce::dsp::ProcessSpec({sampleRate, (uint32_t)numSamples, (uint32_t)1}));
            i.reset();
            i.setMaximumDelayInSamples((int)sampleRate);
            i.setDelay(0.0f);
        }

        delayTimeSmooth.clear();
        delayTimeSmooth.resize(numChannels);
        for (auto& i : delayTimeSmooth)
            i.reset(sampleRate, 0.4f);

        isPrepared.store(true, std::memory_order_release);
    }

    void process(float** buffer, int numChannels, int numSamples, double sampleRate)
    {
        if (this->numChannels != numChannels || this->numSamples != numSamples || this->sampleRate != sampleRate)
        {
            this->numChannels = numChannels;
            this->numSamples = numSamples;
            this->sampleRate = sampleRate;

            isPrepared.store(false, std::memory_order_release);

            return;
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return;

        for (int i = 0; i < numChannels; ++i)
        {
            for (int j = 0; j < numSamples; ++j)
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
