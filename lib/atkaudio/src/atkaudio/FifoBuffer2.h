#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

constexpr auto ATK_CORRECTION_RATE = 1 + 1000.0f / 1000000; // 1000 ppm
constexpr auto ATK_SMOOTHING_TIME = 1.0f;                   // 1 second

class FifoBuffer2 : public juce::Timer
{
public:
    FifoBuffer2()
        : minNumChannels(2)
        , minBufferSize(2048)
        , buffer(minNumChannels, minBufferSize + 1)
        , fifo(minBufferSize + 1)
    {
        timerCallback();
        startTimer(10);
    }

    void timerCallback() override
    {
        if (isPrepared.load(std::memory_order_acquire))
            return;

        juce::ScopedLock lock1(writeLock);
        juce::ScopedLock lock2(readLock);

        fifo.reset();
        auto freeSpace = fifo.getFreeSpace();

        if (freeSpace < (int)std::ceil(minBufferSize))
            freeSpace = 2 * freeSpace;

        if (buffer.getNumChannels() < minNumChannels)
            minNumChannels = 2 * minNumChannels;

        setSize(minNumChannels, freeSpace);

        isPrepared.store(true, std::memory_order_release);
    }

    int write(const float* const* src, int numChannels, int numSamples)
    {
        juce::ScopedTryLock lock(writeLock);
        if (!lock.isLocked())
            return 0;

        if (minBufferSize < numSamples || minNumChannels < numChannels)
        {
            isPrepared.store(false, std::memory_order_release);

            minBufferSize = std::max(minBufferSize, numSamples);
            minNumChannels = std::max(minNumChannels, numChannels);
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        numChannels = std::min(numChannels, buffer.getNumChannels());

        int start1, size1, start2, size2;
        int written = 0;
        fifo.prepareToWrite(numSamples, start1, size1, start2, size2);

        if (size1 > 0)
        {
            for (int ch = 0; ch < numChannels && ch < buffer.getNumChannels(); ++ch)
                buffer.copyFrom(ch, start1, src[ch], size1);
            written += size1;
        }
        if (size2 > 0)
        {
            for (int ch = 0; ch < numChannels && ch < buffer.getNumChannels(); ++ch)
                buffer.copyFrom(ch, start2, src[ch] + size1, size2);
            written += size2;
        }

        // jassert(written == numSamples);

        fifo.finishedWrite(written);
        return written;
    }

    int read(float* const* dest, int numChannels, int numSamples, bool advanceRead = true, bool addToBuffer = false)
    {
        juce::ScopedTryLock lock(readLock);
        if (!lock.isLocked())
            return 0;

        if (minBufferSize < numSamples || minNumChannels < numChannels)
        {
            isPrepared.store(false, std::memory_order_release);

            minBufferSize = std::max(minBufferSize, numSamples);
            minNumChannels = std::max(minNumChannels, numChannels);
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        numChannels = std::min(numChannels, buffer.getNumChannels());

        int start1, size1, start2, size2;
        int readCount = 0;
        fifo.prepareToRead(numSamples, start1, size1, start2, size2);

        if (size1 > 0)
        {
            if (addToBuffer)
            {
                for (int ch = 0; ch < numChannels && ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < size1; ++i)
                        dest[ch][readCount + i] += buffer.getReadPointer(ch, start1)[i];
            }
            else
            {
                for (int ch = 0; ch < numChannels && ch < buffer.getNumChannels(); ++ch)
                    std::memcpy(dest[ch], buffer.getReadPointer(ch, start1), sizeof(float) * size1);
            }

            readCount += size1;
        }
        if (size2 > 0)
        {
            if (addToBuffer)
            {
                for (int ch = 0; ch < numChannels && ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < size2; ++i)
                        dest[ch][readCount + i] += buffer.getReadPointer(ch, start2)[i];
            }
            else
            {
                for (int ch = 0; ch < numChannels && ch < buffer.getNumChannels(); ++ch)
                    std::memcpy(dest[ch] + readCount, buffer.getReadPointer(ch, start2), sizeof(float) * size2);
            }

            readCount += size2;
        }

        if (advanceRead)
            fifo.finishedRead(readCount);

        return readCount;
    }

    void advanceRead(int numSamples)
    {
        fifo.finishedRead(numSamples);
    }

    auto& getBuffer()
    {
        return buffer;
    }

    auto& getFifo()
    {
        return fifo;
    }

private:
    void reset()
    {
        buffer.clear();
        fifo.reset();
    }

    void setSize(int numChannels, int numSamples)
    {
        buffer.setSize(numChannels, numSamples + 1, false, false, true);
        fifo.setTotalSize(numSamples + 1);
        reset();
    }

private:
    int minBufferSize;
    int minNumChannels;

    juce::AudioBuffer<float> buffer;
    juce::AbstractFifo fifo;

    juce::CriticalSection writeLock;
    juce::CriticalSection readLock;
    std::atomic_bool isPrepared{false};
};

class SyncBuffer : public juce::Timer
{
public:
    SyncBuffer()
        : isPrepared(false)
        , readerBufferSize(-1)
        , writerBufferSize(-1)
    {
        startTimer(10);
    }

    void timerCallback() override
    {
        if (isPrepared.load(std::memory_order_acquire))
            return;

        prepare();
    }

    void clearPrepared()
    {
        juce::ScopedLock lock1(writeLock);
        juce::ScopedLock lock2(readLock);

        readerBufferSize = -1;
        writerBufferSize = -1;
        isPrepared.store(false, std::memory_order_release);
    }

    void prepare()
    {
        juce::ScopedLock lock1(writeLock);
        juce::ScopedLock lock2(readLock);

        isPrepared.store(false, std::memory_order_release);

        if (readerNumChannels < 1 || writerNumChannels < 1 || readerBufferSize < 1 || writerBufferSize < 1 ||
            readerSampleRate <= 0.0 || writerSampleRate <= 0.0)
        {
            return;
        }

        numChannels = std::max(readerNumChannels, writerNumChannels);

        interpolators.resize(numChannels);
        for (auto& interp : interpolators)
            interp.reset();

        auto ratio = writerSampleRate / readerSampleRate;

        minBufferSize = 2 * readerBufferSize;
        maxBufferSize = std::max(
            3 * readerBufferSize,
            (int)std::ceil((2 * writerBufferSize) / ratio) // reader domain
        );

        tempBuffer.setSize(numChannels, 2 * std::max(readerBufferSize, writerBufferSize), false, false, true);

        rateSmoothing.reset(readerSampleRate, ATK_SMOOTHING_TIME);
        rateSmoothing.setCurrentAndTargetValue(1.0);
        rateSmoothingBuffer.setSize(numChannels, readerBufferSize, false, false, true);

        tempBuffer.clear();
        fifoBuffer.write(tempBuffer.getArrayOfWritePointers(), numChannels, minBufferSize);

        isPrepared.store(true, std::memory_order_release);
    }

    int write(const float* const* src, int numChannels, int numSamples, double sampleRate)
    {
        juce::ScopedTryLock lock(writeLock);
        if (!lock.isLocked())
            return 0;

        auto newMinBufferSize = numSamples;
        if (writerNumChannels < numChannels || writerBufferSize < newMinBufferSize || writerSampleRate != sampleRate)
        {
            isPrepared.store(false, std::memory_order_release);

            writerNumChannels = numChannels;
            writerBufferSize = newMinBufferSize;
            writerSampleRate = sampleRate;
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        auto written = fifoBuffer.write(src, numChannels, numSamples);

        return written;
    }

    bool read(float* const* dest, int numChannels, int numSamples, double sampleRate, bool addToBuffer = false)
    {
        juce::ScopedTryLock lock(readLock);
        if (!lock.isLocked())
            return false;

        if (readerNumChannels < numChannels || readerSampleRate != sampleRate || readerBufferSize < numSamples)
        {
            isPrepared.store(false, std::memory_order_release);

            readerBufferSize = numSamples;
            readerNumChannels = numChannels;
            readerSampleRate = sampleRate;
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        auto ratio = writerSampleRate / readerSampleRate;

        int maxAvailable = fifoBuffer.getFifo().getNumReady();
        maxAvailable = std::floor(maxAvailable / ratio); // reader domain samples

        auto factor = 1.0f;
        if (maxAvailable < minBufferSize)
            factor = 1 / ATK_CORRECTION_RATE;
        else if (maxAvailable > maxBufferSize)
            factor = ATK_CORRECTION_RATE;

        rateSmoothing.setTargetValue(factor);
        rateSmoothingBuffer.setSize(1, numSamples, false, false, true);
        float maxFactor = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            auto currentFactor = rateSmoothing.getNextValue();
            if (currentFactor > maxFactor)
                maxFactor = currentFactor;
            rateSmoothingBuffer.getWritePointer(0)[i] = currentFactor;
        }

        int writerSamplesNeeded = std::ceil(numSamples * ratio * maxFactor);

        if (!addToBuffer)
            for (int ch = 0; ch < numChannels; ++ch)
                std::memset(dest[ch], 0, sizeof(float) * numSamples);

        tempBuffer.setSize(numChannels, writerSamplesNeeded, false, false, true);
        auto writerSamples =
            fifoBuffer.read(tempBuffer.getArrayOfWritePointers(), numChannels, writerSamplesNeeded, false);

        if (writerSamples < writerSamplesNeeded)
        {
#ifdef JUCE_DEBUG
            DBG("got " << writerSamples << " needed " << writerSamplesNeeded << " ratio " << ratio);
#endif
            return false;
        }

        if (!addToBuffer)
            for (int ch = 0; ch < numChannels; ++ch)
                std::memset(dest[ch], 0, sizeof(float) * numSamples);

        auto smoothingReadPtr = rateSmoothingBuffer.getReadPointer(0);
        auto totalSamplesConsumed = 0;

        auto finalRatio = 0.0f;
        for (int i = 0; i < interpolators.size(); i++)
        {
            totalSamplesConsumed = 0;
            int samplesAvailable = writerSamples;

            auto ch = i % numChannels;

            for (int j = 0; j < numSamples; ++j)
            {
                auto currentFactor = smoothingReadPtr[j];
                finalRatio = ratio * currentFactor;

                int samplesConsumed = interpolators[i].processAdding(
                    finalRatio,
                    tempBuffer.getReadPointer(i) + totalSamplesConsumed,
                    dest[ch] + j,
                    1,
                    1.0f
                );
                samplesAvailable -= samplesConsumed;
                totalSamplesConsumed += samplesConsumed;
            }
        }

#ifdef JUCE_DEBUG
        if (!juce::approximatelyEqual(finalRatio, prevFinalRatio) &&
            juce::approximatelyEqual(finalRatio, (float)(writerSampleRate / readerSampleRate)))
            DBG("time: " << juce::Time::getCurrentTime().toString(true, true)
                         << juce::String(" final ratio ") + juce::String(finalRatio));
#endif
        prevFinalRatio = finalRatio;
        fifoBuffer.advanceRead(totalSamplesConsumed);

        return true;
    }

private:
    std::atomic_bool isPrepared;
    int numChannels{0};
    int bufferSize{0};

    FifoBuffer2 fifoBuffer;
    std::vector<juce::LagrangeInterpolator> interpolators;

    juce::AudioBuffer<float> tempBuffer;

    int readerBufferSize{0};
    int writerBufferSize{0};
    int readerNumChannels{0};
    int writerNumChannels{0};

    double readerSampleRate{0.0};
    double writerSampleRate{0.0};

    int minBufferSize{0};
    int maxBufferSize{0};

    juce::LinearSmoothedValue<float> rateSmoothing;
    juce::AudioBuffer<float> rateSmoothingBuffer;

    juce::CriticalSection readLock;
    juce::CriticalSection writeLock;

    float prevFinalRatio{1.0f};
};
