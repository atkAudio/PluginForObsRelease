#include "ParentSourceBridge.h"
#include <atkaudio/AudioProcessorGraphMT/RealtimeSafety.h>

namespace atk
{
void ParentSourceBridge::prepareBuffers()
{
    input.reset();
    output.reset();
    input.prepare(preparedChannels, preparedBlockSize, preparedSampleRate);
    output.prepare(preparedChannels, preparedBlockSize, preparedSampleRate);
}

void ParentSourceBridge::start(Mode newMode, int channels, int blockSize, double sampleRate)
{
    std::unique_lock lock(lifecycleLock);
    mode.store(stopped, std::memory_order_release);
    preparedChannels = channels;
    preparedBlockSize = blockSize;
    preparedSampleRate = sampleRate;
    parentChannels.store(channels, std::memory_order_release);
    parentSampleRate = 0.0;
    if (channels > 0 && blockSize > 0 && sampleRate > 0.0 && newMode == hardware)
        prepareBuffers();
    else if (newMode == hardware)
        return;
    mode.store(newMode, std::memory_order_release);
    sendChangeMessage();
}

void ParentSourceBridge::stop()
{
    std::unique_lock lock(lifecycleLock);
    mode.store(stopped, std::memory_order_release);
    input.clearPrepared();
    output.clearPrepared();
    sendChangeMessage();
}

void ParentSourceBridge::reset()
{
    std::unique_lock lock(lifecycleLock);
    generation.fetch_add(1, std::memory_order_release);
    if (mode.load(std::memory_order_relaxed) == hardware)
        prepareBuffers();
}

ParentSourceBridge::Mode ParentSourceBridge::getMode()
{
    return mode.load(std::memory_order_acquire);
}

int ParentSourceBridge::getParentChannels()
{
    return parentChannels.load(std::memory_order_acquire);
}

uint64_t ParentSourceBridge::getGeneration()
{
    return generation.load(std::memory_order_acquire);
}

bool ParentSourceBridge::processParent(float* const* samples, int channels, int frames, double sampleRate)
{
    return processParent(samples, channels, frames, sampleRate, juce::Time::getMillisecondCounterHiRes() * 0.001);
}

bool ParentSourceBridge::processParent(
    float* const* samples,
    int channels,
    int frames,
    double sampleRate,
    double timeSeconds
)
{
    std::shared_lock lock(lifecycleLock, std::try_to_lock);
    std::unique_lock formatLock(lifecycleLock, std::defer_lock);
    if (lock.owns_lock() && getMode() == obsAudio)
        return false;

    if (lock.owns_lock() && getMode() == hardware && channels > 0 && frames > 0)
    {
        if (channels != preparedChannels || (parentSampleRate != 0.0 && parentSampleRate != sampleRate))
        {
            lock.unlock();
            if (!formatLock.try_lock() || getMode() != hardware)
            {
                for (int channel = 0; channel < channels; ++channel)
                    juce::FloatVectorOperations::clear(samples[channel], frames);
                return true;
            }
            preparedChannels = channels;
            prepareBuffers();
            parentChannels.store(channels, std::memory_order_release);
            sendChangeMessage();
        }
        parentSampleRate = sampleRate;
        input.write(samples, channels, frames, sampleRate, timeSeconds);
        output.read(samples, channels, frames, sampleRate, timeSeconds);
    }
    else
    {
        for (int channel = 0; channel < channels; ++channel)
            juce::FloatVectorOperations::clear(samples[channel], frames);
    }
    return true;
}

void ParentSourceBridge::readInput(juce::AudioBuffer<float>& samples, double sampleRate)
{
    readInput(samples, sampleRate, juce::Time::getMillisecondCounterHiRes() * 0.001);
}

void ParentSourceBridge::readInput(juce::AudioBuffer<float>& samples, double sampleRate, double timeSeconds)
{
    readInput(samples, sampleRate, timeSeconds, getGeneration());
}

void ParentSourceBridge::readInput(
    juce::AudioBuffer<float>& samples,
    double sampleRate,
    double timeSeconds,
    uint64_t expectedGeneration
)
{
    [[maybe_unused]] ScopedRealtimeSanitizerDisabler disabler;
    std::shared_lock lock(lifecycleLock, std::try_to_lock);
    if (lock.owns_lock()
        && getMode() == hardware
        && samples.getNumChannels() > 0
        && expectedGeneration == getGeneration())
        input.read(
            samples.getArrayOfWritePointers(),
            samples.getNumChannels(),
            samples.getNumSamples(),
            sampleRate,
            timeSeconds
        );
    else
        samples.clear();
}

void ParentSourceBridge::writeOutput(juce::AudioBuffer<float>& samples, double sampleRate)
{
    writeOutput(samples, sampleRate, juce::Time::getMillisecondCounterHiRes() * 0.001);
}

void ParentSourceBridge::writeOutput(juce::AudioBuffer<float>& samples, double sampleRate, double timeSeconds)
{
    writeOutput(samples, sampleRate, timeSeconds, getGeneration());
}

void ParentSourceBridge::writeOutput(
    juce::AudioBuffer<float>& samples,
    double sampleRate,
    double timeSeconds,
    uint64_t expectedGeneration
)
{
    [[maybe_unused]] ScopedRealtimeSanitizerDisabler disabler;
    std::shared_lock lock(lifecycleLock, std::try_to_lock);
    if (lock.owns_lock()
        && getMode() == hardware
        && samples.getNumChannels() > 0
        && expectedGeneration == getGeneration())
    {
        output.write(
            samples.getArrayOfReadPointers(),
            samples.getNumChannels(),
            samples.getNumSamples(),
            sampleRate,
            timeSeconds
        );
    }
}
} // namespace atk
