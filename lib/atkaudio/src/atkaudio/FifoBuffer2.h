#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#define ATK_RATE_FACTOR 1.001f
#define ATK_SMOOTHING_SPEED 1.0f

class FifoBuffer2
{
public:
    FifoBuffer2()
        : buffer()
        , fifo(1)
    {
    }

    void setSize(int numChannels, int numSamples)
    {
        buffer.setSize(numChannels, numSamples, false, false, true);
        fifo.setTotalSize(numSamples);
        this->reset();
    }

    int write(const float* const* src, int numChannels, int numSamples)
    {
        jassert(buffer.getNumSamples() >= numSamples);
        jassert(buffer.getNumChannels() >= numChannels);

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

        fifo.finishedWrite(written);
        return written;
    }

    int read(float* const* dest, int numChannels, int numSamples, bool advanceRead = true, bool addToBuffer = false)
    {
        jassert(buffer.getNumSamples() >= numSamples);
        jassert(buffer.getNumChannels() >= numChannels);
        jassert(fifo.getNumReady() >= numSamples);

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

        jassert(readCount == numSamples);

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

    void reset()
    {
        buffer.clear();
        fifo.reset();
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
    juce::AudioBuffer<float> buffer;
    juce::AbstractFifo fifo;
};

class SyncBuffer : public juce::Timer
{
public:
    SyncBuffer()
        : isPrepared(false)
        , readerBufferSize(-1)
        , writerBufferSize(-1)
    {
        startTimerHz(60);
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

        fifoBuffer.setSize(numChannels, 2 * maxBufferSize);

        rateSmoothing.reset(
            readerSampleRate.load(std::memory_order_acquire) / readerBufferSize.load(std::memory_order_acquire) * 1.0,
            ATK_SMOOTHING_SPEED
        );
        rateSmoothing.setCurrentAndTargetValue(1.0);

        tempBuffer.clear();
        for (int ch = 0; ch < numChannels; ++ch)
            tempBuffer.clear(ch, 0, minBufferSize);
        fifoBuffer.write(tempBuffer.getArrayOfWritePointers(), numChannels, minBufferSize);

        isPrepared.store(true, std::memory_order_release);
    }

    void prepareReader(double sampleRate, int newNumChannels, int bufferSize)
    {
        readerSampleRate.store(sampleRate, std::memory_order_release);
        readerBufferSize.store(bufferSize, std::memory_order_release);
        readerNumChannels.store(newNumChannels, std::memory_order_release);
    }

    void prepareWriter(double sampleRate, int newNumChannels, int bufferSize)
    {
        writerSampleRate.store(sampleRate, std::memory_order_release);
        writerBufferSize.store(bufferSize, std::memory_order_release);
        writerNumChannels.store(newNumChannels, std::memory_order_release);
    }

    int write(const float* const* src, int numChannels, int numSamples)
    {
        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        juce::ScopedLock lock(writeLock);

        fifoBuffer.write(src, numChannels, numSamples);

        return numSamples;
    }

    int read(float* const* dest, int numChannels, int numSamples, bool addToBuffer = false)
    {
        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        juce::ScopedLock lock(readLock);

        auto ratio =
            writerSampleRate.load(std::memory_order_acquire) / readerSampleRate.load(std::memory_order_acquire);

        int maxAvailable = fifoBuffer.getNumReady();

        auto factor = 1.0f;
        if (maxAvailable < std::ceil(ATK_RATE_FACTOR * numSamples))
        {
            factor = factor / ATK_RATE_FACTOR;
        }
        else if (maxAvailable > 2 * std::max(
                                        std::ceil(ATK_RATE_FACTOR * readerBufferSize.load(std::memory_order_acquire)),
                                        (float)writerBufferSize.load(std::memory_order_acquire)
                                    ))
        {
            factor = factor * ATK_RATE_FACTOR;
        }

        rateSmoothing.setTargetValue(factor);
        factor = rateSmoothing.getNextValue();

        ratio = ratio * factor;

        int samplesNeeded = std::ceil(numSamples * ratio);

        if (maxAvailable < samplesNeeded)
            return 0;

        // #if defined(JUCE_DEBUG) && defined(JUCE_WINDOWS)
        //         DBG(ratio);
        // #endif

        auto samplesGot = fifoBuffer.read(tempBuffer.getArrayOfWritePointers(), numChannels, samplesNeeded, false);

        int samplesConsumed = 0;

        for (int i = 0; i < interpolators.size(); i++)
        {
            auto ch = i % numChannels;
            if (addToBuffer || ch >= interpolators.size())
                samplesConsumed = interpolators[i].processAdding(
                    ratio,
                    tempBuffer.getReadPointer(ch),
                    dest[ch],
                    numSamples,
                    maxAvailable,
                    maxAvailable,
                    1.0f
                );
            else
                samplesConsumed = interpolators[i].process(
                    ratio,
                    tempBuffer.getReadPointer(ch),
                    dest[ch],
                    numSamples,
                    maxAvailable,
                    maxAvailable
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
    // std::vector<float*> tempPtrs;

    std::atomic<int> readerBufferSize;
    std::atomic<int> writerBufferSize;
    std::atomic<int> readerNumChannels{0};
    std::atomic<int> writerNumChannels{0};

    std::atomic<double> readerSampleRate{0.0};
    std::atomic<double> writerSampleRate{0.0};

    int minBufferSize{0};
    int maxBufferSize{0};

    juce::LinearSmoothedValue<double> rateSmoothing;

    juce::CriticalSection readLock;
    juce::CriticalSection writeLock;

    // std::atomic_bool isReading = false;
    // std::atomic_bool isWriting = false;
};