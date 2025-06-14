#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#define ATK_RATE_FACTOR 1.001f
#define ATK_SMOOTHING_SPEED 1.0f

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
        startTimer(100);
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
        juce::ScopedLock lock(writeLock);

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
        juce::ScopedLock lock(readLock);
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

    int getNumReady() const
    {
        return fifo.getNumReady();
    }

    int getFreeSpace() const
    {
        return fifo.getFreeSpace();
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
        startTimer(100);
    }

    void timerCallback() override
    {
        if (isPrepared.load(std::memory_order_acquire))
            return;

        if (readerBufferSize.load(std::memory_order_acquire) < 0 ||
            writerBufferSize.load(std::memory_order_acquire) < 0)
        {
            return;
        }

        prepare();
    }

    void clearPrepared()
    {
        readerBufferSize.store(-1, std::memory_order_release);
        writerBufferSize.store(-1, std::memory_order_release);
        isPrepared.store(false, std::memory_order_release);
    }

    void prepare()
    {
        isPrepared.store(false, std::memory_order_release);

        juce::ScopedLock lock1(writeLock);
        juce::ScopedLock lock2(readLock);

        numChannels = std::max(
            readerNumChannels.load(std::memory_order_acquire),
            writerNumChannels.load(std::memory_order_acquire)
        );

        interpolators.resize(numChannels);
        for (auto& interp : interpolators)
            interp.reset();

        auto ratio =
            readerSampleRate.load(std::memory_order_acquire) / writerSampleRate.load(std::memory_order_acquire);

        minBufferSize = (int)std::ceil((ATK_RATE_FACTOR * readerBufferSize.load(std::memory_order_acquire) / ratio));
        maxBufferSize = 3 * std::max(
                                (int)std::ceil((readerBufferSize.load(std::memory_order_acquire) / ratio)),
                                writerBufferSize.load(std::memory_order_acquire)
                            );

        tempBuffer.setSize(numChannels, 2 * maxBufferSize, false, false, true);

        rateSmoothing.reset(
            readerSampleRate.load(std::memory_order_acquire) / readerBufferSize.load(std::memory_order_acquire) * 1.0,
            ATK_SMOOTHING_SPEED
        );
        rateSmoothing.setCurrentAndTargetValue(1.0);

        tempBuffer.clear();
        fifoBuffer.write(tempBuffer.getArrayOfWritePointers(), numChannels, minBufferSize);

        isPrepared.store(true, std::memory_order_release);
    }

    void prepareReader(double sampleRate, int newNumChannels, int bufferSize)
    {
        readerSampleRate.store(sampleRate, std::memory_order_release);
    }

    void prepareWriter(double sampleRate, int newNumChannels, int bufferSize)
    {
        writerSampleRate.store(sampleRate, std::memory_order_release);
    }

    int write(const float* const* src, int numChannels, int numSamples)
    {
        juce::ScopedLock lock(writeLock);

        auto newMinBufferSize = numSamples;
        if (writerNumChannels.load(std::memory_order_acquire) < numChannels ||
            writerBufferSize.load(std::memory_order_acquire) < newMinBufferSize)
        {
            isPrepared.store(false, std::memory_order_release);

            writerNumChannels.store(numChannels, std::memory_order_release);
            writerBufferSize.store(newMinBufferSize, std::memory_order_release);
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        fifoBuffer.write(src, numChannels, numSamples);

        return numSamples;
    }

    int read(float* const* dest, int numChannels, int numSamples, bool addToBuffer = false)
    {
        juce::ScopedLock lock(readLock);
        auto readerMinBufferSizeOverhead = (int)std::ceil(
            (ATK_RATE_FACTOR * readerSampleRate.load(std::memory_order_acquire) -
             readerSampleRate.load(std::memory_order_acquire)) /
            2 * ATK_SMOOTHING_SPEED
        );

        auto readerMinBufferSize = readerMinBufferSizeOverhead + numSamples;

        auto ratio =
            writerSampleRate.load(std::memory_order_acquire) / readerSampleRate.load(std::memory_order_acquire);

        readerMinBufferSize = (int)std::ceil(readerMinBufferSize * ratio);

        if (readerNumChannels.load(std::memory_order_acquire) < numChannels ||
            readerBufferSize.load(std::memory_order_acquire) < readerMinBufferSize)
        {
            isPrepared.store(false, std::memory_order_release);

            readerNumChannels.store(numChannels, std::memory_order_release);
            readerBufferSize.store(readerMinBufferSize, std::memory_order_release);
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        int maxAvailable = fifoBuffer.getNumReady();

        auto factor = 1.0f;
        if (maxAvailable < readerMinBufferSize)
            factor = factor / ATK_RATE_FACTOR;
        else if (maxAvailable > 2 * std::max(readerMinBufferSize, writerBufferSize.load(std::memory_order_acquire)))
            factor = factor * ATK_RATE_FACTOR;

        rateSmoothing.setTargetValue(factor);
        factor = rateSmoothing.getNextValue();

        ratio = ratio * factor;

        int samplesNeeded = std::ceil(numSamples * ratio);

        if (!addToBuffer)
            for (int ch = 0; ch < numChannels; ++ch)
                std::memset(dest[ch], 0, sizeof(float) * numSamples);

#ifdef JUCE_DEBUG
        if (maxAvailable < samplesNeeded)
            DBG("needed " << samplesNeeded << " available " << maxAvailable << " ratio " << ratio);
#endif

        tempBuffer.setSize(numChannels, samplesNeeded, false, false, true);

        auto samplesGot = fifoBuffer.read(tempBuffer.getArrayOfWritePointers(), numChannels, samplesNeeded, false);

        int samplesConsumed = 0;

        for (int i = 0; i < interpolators.size(); i++)
        {
            auto ch = i % numChannels;
            samplesConsumed = interpolators[i].processAdding(
                ratio,
                tempBuffer.getReadPointer(ch),
                dest[ch],
                numSamples,
                maxAvailable,
                maxAvailable,
                1.0f
            );
        }

        fifoBuffer.advanceRead(samplesConsumed);

        return samplesConsumed;
    }

    int getNumReady() const
    {
        return fifoBuffer.getNumReady();
    }

    int getFreeSpace() const
    {
        return fifoBuffer.getFreeSpace();
    }

    double getReaderSampleRate() const
    {
        return readerSampleRate.load(std::memory_order_acquire);
    }

    double getWriterSampleRate() const
    {
        return writerSampleRate.load(std::memory_order_acquire);
    }

    int getReaderBufferSize() const
    {
        return readerBufferSize.load(std::memory_order_acquire);
    }

    int getWriterBufferSize() const
    {
        return writerBufferSize.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool isPrepared;
    int numChannels{0};
    int bufferSize{0};

    FifoBuffer2 fifoBuffer;
    std::vector<juce::LagrangeInterpolator> interpolators;

    juce::AudioBuffer<float> tempBuffer;

    std::atomic<int> readerBufferSize{0};
    std::atomic<int> writerBufferSize{0};
    std::atomic<int> readerNumChannels{0};
    std::atomic<int> writerNumChannels{0};

    std::atomic<double> readerSampleRate{0.0};
    std::atomic<double> writerSampleRate{0.0};

    int minBufferSize{0};
    int maxBufferSize{0};

    juce::LinearSmoothedValue<double> rateSmoothing;

    juce::CriticalSection readLock;
    juce::CriticalSection writeLock;
};