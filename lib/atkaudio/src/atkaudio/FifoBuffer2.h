#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

constexpr double ATK_CORRECTION_RATE = 1.0 + 1000.0 / 1000000.0;
constexpr double ATK_SMOOTHING_TIME = 0.1;

class FifoBuffer2
{
public:
    FifoBuffer2()
        : minNumChannels(2)
        , minBufferSize(2048)
        , buffer(minNumChannels, minBufferSize + 1)
        , fifo(minBufferSize + 1)
    {
        isPrepared.store(true, std::memory_order_release);
    }

    void ensurePrepared()
    {
        if (isPrepared.load(std::memory_order_acquire))
            return;

        juce::ScopedLock lock1(writeLock);
        juce::ScopedLock lock2(readLock);

        if (isPrepared.load(std::memory_order_acquire))
            return;

        fifo.reset();
        auto fifoTotalSize = fifo.getTotalSize();

        if (fifoTotalSize < minBufferSize + 1)
            fifoTotalSize = 2 * minBufferSize;

        if (buffer.getNumChannels() < minNumChannels)
            minNumChannels = 2 * minNumChannels;

        setSize(minNumChannels, fifoTotalSize);

        isPrepared.store(true, std::memory_order_release);
    }

    int write(const float* const* src, int numChannels, int numSamples)
    {
        if (minBufferSize < numSamples || minNumChannels < numChannels)
        {
            isPrepared.store(false, std::memory_order_release);
            minBufferSize = std::max(minBufferSize, numSamples);
            minNumChannels = std::max(minNumChannels, numChannels);

            juce::MessageManager::callAsync([this]() { ensurePrepared(); });

            return 0;
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        juce::ScopedTryLock lock(writeLock);
        if (!lock.isLocked())
            return 0;

        numChannels = std::min(numChannels, buffer.getNumChannels());

        int start1, size1, start2, size2;
        int written = 0;
        fifo.prepareToWrite(numSamples, start1, size1, start2, size2);

        if (size1 > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom(ch, start1, src[ch], size1);
            written += size1;
        }
        if (size2 > 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom(ch, start2, src[ch] + size1, size2);
            written += size2;
        }

        fifo.finishedWrite(written);
        return written;
    }

    int read(float* const* dest, int numChannels, int numSamples, bool advanceRead = true, bool addToBuffer = false)
    {
        if (minBufferSize < numSamples || minNumChannels < numChannels)
        {
            isPrepared.store(false, std::memory_order_release);
            minBufferSize = std::max(minBufferSize, numSamples);
            minNumChannels = std::max(minNumChannels, numChannels);

            juce::MessageManager::callAsync([this]() { ensurePrepared(); });

            return 0;
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        juce::ScopedTryLock lock(readLock);
        if (!lock.isLocked())
            return 0;

        numChannels = std::min(numChannels, buffer.getNumChannels());

        int start1, size1, start2, size2;
        int readCount = 0;
        fifo.prepareToRead(numSamples, start1, size1, start2, size2);

        if (size1 > 0)
        {
            if (addToBuffer)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < size1; ++i)
                        dest[ch][i] += buffer.getReadPointer(ch, start1)[i];
            }
            else
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    juce::FloatVectorOperations::copy(dest[ch], buffer.getReadPointer(ch, start1), size1);
            }

            readCount += size1;
        }
        if (size2 > 0)
        {
            if (addToBuffer)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < size2; ++i)
                        dest[ch][readCount + i] += buffer.getReadPointer(ch, start2)[i];
            }
            else
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    juce::FloatVectorOperations::copy(dest[ch] + readCount, buffer.getReadPointer(ch, start2), size2);
            }

            readCount += size2;
        }

        if (advanceRead)
            this->advanceRead(readCount);

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
        numSamples = numSamples > 16384 ? numSamples : 16384;
        if (buffer.getNumSamples() < numSamples + 1)
            buffer.setSize(numChannels, numSamples + 1, false, false, true);

        if (fifo.getTotalSize() < numSamples + 1)
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

// SyncBuffer: Sample rate converting buffer with dynamic rate correction
class SyncBuffer
{
public:
    SyncBuffer()
        : isPrepared(false)
        , readerBufferSize(-1)
        , writerBufferSize(-1)
    {
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

        if ((readerNumChannels < 1)
            || (writerNumChannels < 1)
            || (readerBufferSize < 1)
            || (writerBufferSize < 1)
            || (readerSampleRate <= 0.0)
            || (writerSampleRate <= 0.0))
        {
            return;
        }

        numChannels = std::max(readerNumChannels, writerNumChannels);

        interpolators.resize(writerNumChannels);
        for (auto& interp : interpolators)
            interp.reset();

        auto ratio = writerSampleRate / readerSampleRate;

        minBufferSize = static_cast<int>(std::ceil(1.0 * readerBufferSize * ratio));

        int maxBasedOnReader = static_cast<int>(std::ceil(3.0 * readerBufferSize * ratio));
        int maxBasedOnWriter = static_cast<int>(std::ceil(2.0 * writerBufferSize));
        maxBufferSize = std::max(maxBasedOnReader, maxBasedOnWriter);

        int maxTempBufferNeeded = static_cast<int>(maxBufferSize);
        tempBuffer.setSize(numChannels, maxTempBufferNeeded, false, false, true);

        rateSmoothing.reset(static_cast<int>(readerSampleRate * ATK_SMOOTHING_TIME));
        rateSmoothing.setCurrentAndTargetValue(1.0);

        tempBuffer.clear();
        fifoBuffer.getFifo().reset();

        int preFillSize = minBufferSize;
        fifoBuffer.write(tempBuffer.getArrayOfWritePointers(), numChannels, preFillSize);

        isPrepared.store(true, std::memory_order_release);
    }

    int write(const float* const* src, int numChannels, int numSamples, double sampleRate)
    {
        auto newMinBufferSize = numSamples;
        if (writerNumChannels < numChannels || writerBufferSize < newMinBufferSize || writerSampleRate != sampleRate)
        {
            bool expected = false;
            if (isPreparing.compare_exchange_strong(expected, true, std::memory_order_acquire))
            {
                isPrepared.store(false, std::memory_order_release);

                writerNumChannels = numChannels;
                writerBufferSize = newMinBufferSize;
                writerSampleRate = sampleRate;

                prepare();

                isPreparing.store(false, std::memory_order_release);

                if (!isPrepared.load(std::memory_order_acquire))
                    return 0;
            }
            else
            {
                return 0;
            }
        }

        if (!isPrepared.load(std::memory_order_acquire))
        {
            bool expected = false;
            if (isPreparing.compare_exchange_strong(expected, true, std::memory_order_acquire))
            {
                prepare();
                isPreparing.store(false, std::memory_order_release);

                if (!isPrepared.load(std::memory_order_acquire))
                    return 0;
            }
            else
            {
                return 0;
            }
        }

        juce::ScopedTryLock lock(writeLock);
        if (!lock.isLocked())
            return 0;

        auto written = fifoBuffer.write(src, numChannels, numSamples);

        return written;
    }

    bool read(float* const* dest, int numChannels, int numSamples, double sampleRate, bool addToBuffer = false)
    {
        if (readerNumChannels < numChannels || readerSampleRate != sampleRate || readerBufferSize < numSamples)
        {
            bool expected = false;
            if (isPreparing.compare_exchange_strong(expected, true, std::memory_order_acquire))
            {
                isPrepared.store(false, std::memory_order_release);

                readerBufferSize = numSamples;
                readerNumChannels = numChannels;
                readerSampleRate = sampleRate;

                prepare();

                isPreparing.store(false, std::memory_order_release);

                if (!isPrepared.load(std::memory_order_acquire))
                    return false;
            }
            else
            {
                return false;
            }
        }

        if (!isPrepared.load(std::memory_order_acquire))
        {
            bool expected = false;
            if (isPreparing.compare_exchange_strong(expected, true, std::memory_order_acquire))
            {
                prepare();
                isPreparing.store(false, std::memory_order_release);

                if (!isPrepared.load(std::memory_order_acquire))
                    return false;
            }
            else
            {
                return false;
            }
        }

        juce::ScopedTryLock lock(readLock);
        if (!lock.isLocked())
            return false;

        auto ratio = writerSampleRate / readerSampleRate;

        int samplesInFifo = fifoBuffer.getFifo().getNumReady();

        int targetBufferSize = minBufferSize;

        auto factor = 1.0;
        static bool wasUnderflow = false;
        static bool wasOverflow = false;

        if (samplesInFifo < minBufferSize)
        {
            double availableRatio = static_cast<double>(samplesInFifo) / static_cast<double>(numSamples * ratio);
            factor = std::max(0.5, availableRatio);
            wasUnderflow = true;
        }
        else if (samplesInFifo > maxBufferSize)
        {
            factor = ATK_CORRECTION_RATE;
            wasOverflow = true;
        }
        else
        {
            if (wasUnderflow)
            {
                DBG("SyncBuffer: Recovered from UNDERFLOW at "
                    << juce::Time::getCurrentTime().toString(true, true, true, true)
                    << ", samples in FIFO: "
                    << samplesInFifo);
                wasUnderflow = false;
            }
            if (wasOverflow)
            {
                DBG("SyncBuffer: Recovered from OVERFLOW at "
                    << juce::Time::getCurrentTime().toString(true, true, true, true)
                    << ", samples in FIFO: "
                    << samplesInFifo);
                wasOverflow = false;
            }
        }

        auto initialRate = rateSmoothing.getCurrentValue();

        auto savedSmoothing = rateSmoothing;

        int writerSamplesNeeded = 0;
        double writerSamplesNeededTemp = 0;
        rateSmoothing.setTargetValue(factor);
        for (int i = 0; i < numSamples; ++i)
            writerSamplesNeededTemp += ratio * rateSmoothing.getNextValue();

        writerSamplesNeeded = static_cast<int>(std::ceil(writerSamplesNeededTemp));

        rateSmoothing = savedSmoothing;

        if (!addToBuffer)
            for (int ch = 0; ch < numChannels; ++ch)
                std::memset(dest[ch], 0, sizeof(float) * numSamples);

        if (tempBuffer.getNumSamples() < writerSamplesNeeded)
            tempBuffer.setSize(writerNumChannels, writerSamplesNeeded, true, false, true);

        tempBuffer.clear();

        auto writerSamples =
            fifoBuffer.read(tempBuffer.getArrayOfWritePointers(), writerNumChannels, writerSamplesNeeded, false);

        if (writerSamples < writerSamplesNeeded)
        {
            DBG("SyncBuffer: UNDERRUN - got "
                << writerSamples
                << " samples, needed "
                << writerSamplesNeeded
                << ", ratio "
                << ratio
                << ", factor "
                << factor);
            return false;
        }

        rateSmoothing.setCurrentAndTargetValue(initialRate);
        rateSmoothing.setTargetValue(factor);

        auto finalRatio = 0.0;
        int maxSamplesConsumed = 0;

        auto channelGain = 1.0f;
        if (writerNumChannels > numChannels)
            channelGain = static_cast<float>(std::sqrt(static_cast<double>(numChannels) / writerNumChannels));

        for (int destCh = 0; destCh < numChannels; ++destCh)
        {
            int totalSamplesConsumed = 0;
            int samplesAvailable = writerSamples;

            int srcCh = destCh % writerNumChannels;

            rateSmoothing.setCurrentAndTargetValue(initialRate);
            rateSmoothing.setTargetValue(factor);

            for (int j = 0; j < numSamples; ++j)
            {
                auto smoothingValue = rateSmoothing.getNextValue();
                finalRatio = ratio * smoothingValue;

                int samplesConsumed = interpolators[srcCh].process(
                    finalRatio,
                    tempBuffer.getReadPointer(srcCh) + totalSamplesConsumed,
                    dest[destCh] + j,
                    1,
                    samplesAvailable,
                    0
                );

                dest[destCh][j] *= channelGain;

                samplesAvailable -= samplesConsumed;
                totalSamplesConsumed += samplesConsumed;
            }

            maxSamplesConsumed = std::max(maxSamplesConsumed, totalSamplesConsumed);
        }

        if (writerNumChannels > numChannels)
        {
            for (int i = numChannels; i < writerNumChannels; ++i)
            {
                int totalSamplesConsumed = 0;
                int samplesAvailable = writerSamples;

                auto destCh = i % numChannels;

                rateSmoothing.setCurrentAndTargetValue(initialRate);
                rateSmoothing.setTargetValue(factor);

                for (int j = 0; j < numSamples; ++j)
                {
                    auto smoothingValue = rateSmoothing.getNextValue();
                    finalRatio = ratio * smoothingValue;

                    float sample = 0.0f;
                    int samplesConsumed = interpolators[i].process(
                        finalRatio,
                        tempBuffer.getReadPointer(i) + totalSamplesConsumed,
                        &sample,
                        1,
                        samplesAvailable,
                        0
                    );

                    dest[destCh][j] += sample * channelGain;

                    samplesAvailable -= samplesConsumed;
                    totalSamplesConsumed += samplesConsumed;
                }

                maxSamplesConsumed = std::max(maxSamplesConsumed, totalSamplesConsumed);
            }
        }

        if (!juce::approximatelyEqual(finalRatio, prevFinalRatio)
            && juce::approximatelyEqual(finalRatio, writerSampleRate / readerSampleRate))
            DBG("SyncBuffer: Rate stabilized at "
                << juce::Time::getCurrentTime().toString(true, true)
                << ", final ratio: "
                << finalRatio);

        prevFinalRatio = finalRatio;
        fifoBuffer.advanceRead(maxSamplesConsumed);

        return true;
    }

private:
    std::atomic_bool isPrepared{false};
    std::atomic_bool isPreparing{false};
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

    juce::LinearSmoothedValue<double> rateSmoothing;

    juce::CriticalSection readLock;
    juce::CriticalSection writeLock;

    double prevFinalRatio{1.0};
};
