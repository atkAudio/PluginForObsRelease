#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include "FifoBuffer.h"

#include <juce_core/juce_core.h>

#if defined(ATK_DEBUG) && !defined(DBG)
#define DBG(x) std::cerr << x << std::endl
#endif

static constexpr auto FIXED_BUFFER_SIZE = 65536;
static constexpr auto TARGET_LEVEL_FACTOR = 1.5;

namespace atk
{

class LagrangeInterpolator
{
public:
    LagrangeInterpolator() noexcept
    {
        reset();
    }

    void reset() noexcept
    {
        subSamplePos = 1.0;
        indexBuffer = 0;
        for (int i = 0; i < 5; ++i)
            lastInputSamples[i] = 0.0f;
    }

    int process(
        double speedRatio,
        const float* inputSamples,
        float* outputSamples,
        int numOutputSamples,
        int numInputSamples,
        int wrapAround
    ) noexcept
    {
        (void)wrapAround; // Unused - reserved for future use
        if (speedRatio <= 0.0)
            return 0;

        int pos = 0;
        auto* out = outputSamples;

        while (numOutputSamples > 0)
        {
            while (subSamplePos >= 1.0)
            {
                if (pos >= numInputSamples)
                    return pos;

                pushInterpolationSample(inputSamples[pos++]);
                subSamplePos -= 1.0;
            }

            *out++ = interpolate();
            subSamplePos += speedRatio;
            --numOutputSamples;
        }

        return pos;
    }

    int processAdding(
        double speedRatio,
        const float* inputSamples,
        float* outputSamples,
        int numOutputSamples,
        int numInputSamples,
        int wrapAround,
        float gain
    ) noexcept
    {
        (void)wrapAround; // Unused - reserved for future use
        if (speedRatio <= 0.0)
            return 0;

        int pos = 0;
        auto* out = outputSamples;

        while (numOutputSamples > 0)
        {
            while (subSamplePos >= 1.0)
            {
                if (pos >= numInputSamples)
                    return pos;

                pushInterpolationSample(inputSamples[pos++]);
                subSamplePos -= 1.0;
            }

            *out++ += gain * interpolate();
            subSamplePos += speedRatio;
            --numOutputSamples;
        }

        return pos;
    }

private:
    void pushInterpolationSample(float newValue) noexcept
    {
        lastInputSamples[indexBuffer] = newValue;
        if (++indexBuffer == 5)
            indexBuffer = 0;
    }

    template <int k>
    static float calcCoefficient(float input, float offset) noexcept
    {
        if constexpr (k != 0)
            input *= (-2.0f - offset) * (1.0f / (0 - k));
        if constexpr (k != 1)
            input *= (-1.0f - offset) * (1.0f / (1 - k));
        if constexpr (k != 2)
            input *= (0.0f - offset) * (1.0f / (2 - k));
        if constexpr (k != 3)
            input *= (1.0f - offset) * (1.0f / (3 - k));
        if constexpr (k != 4)
            input *= (2.0f - offset) * (1.0f / (4 - k));
        return input;
    }

    float interpolate() const noexcept
    {
        auto offset = static_cast<float>(subSamplePos);
        auto index = indexBuffer;

        float result = 0.0f;
        result += calcCoefficient<0>(lastInputSamples[index], offset);
        if (++index == 5)
            index = 0;
        result += calcCoefficient<1>(lastInputSamples[index], offset);
        if (++index == 5)
            index = 0;
        result += calcCoefficient<2>(lastInputSamples[index], offset);
        if (++index == 5)
            index = 0;
        result += calcCoefficient<3>(lastInputSamples[index], offset);
        if (++index == 5)
            index = 0;
        result += calcCoefficient<4>(lastInputSamples[index], offset);

        return result;
    }

    float lastInputSamples[5];
    double subSamplePos;
    int indexBuffer{0};
};

} // namespace atk

class FifoBuffer2
{
public:
    FifoBuffer2()
    {
        buffer.setSize(2, 2);
    }

    void setSize(int numChannels, int numSamples)
    {
        std::lock_guard<std::mutex> lock1(writeLock);
        std::lock_guard<std::mutex> lock2(readLock);

        buffer.setSize(numChannels, numSamples + 1);
    }

    int write(const float* const* src, int numChannels, int numSamples)
    {
        std::unique_lock<std::mutex> lock(writeLock, std::try_to_lock);
        if (!lock.owns_lock())
            return 0;

        numChannels = std::min(numChannels, buffer.getNumChannels());

        int freeSpace = buffer.getFreeSpace();
        int toWrite = std::min(numSamples, freeSpace);

        if (toWrite <= 0)
            return 0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            bool isLastChannel = (ch == numChannels - 1);
            buffer.write(src[ch], ch, toWrite, isLastChannel);
        }

        return toWrite;
    }

    int read(float* const* dest, int numChannels, int numSamples, bool advanceReadPos = true, bool addToBuffer = false)
    {
        std::unique_lock<std::mutex> lock(readLock, std::try_to_lock);
        if (!lock.owns_lock())
            return 0;

        numChannels = std::min(numChannels, buffer.getNumChannels());

        int available = buffer.getNumReady();
        int toRead = std::min(numSamples, available);

        if (toRead <= 0)
            return 0;

        if (addToBuffer)
        {
            if (tempReadBuffer.size() < static_cast<size_t>(toRead))
                tempReadBuffer.resize(toRead);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                buffer.read(tempReadBuffer.data(), ch, toRead, false);
                for (int i = 0; i < toRead; ++i)
                    dest[ch][i] += tempReadBuffer[i];
            }
        }
        else
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.read(dest[ch], ch, toRead, false);
        }

        if (advanceReadPos)
            buffer.advanceRead(toRead);

        return toRead;
    }

    void advanceRead(int numSamples)
    {
        buffer.advanceRead(numSamples);
    }

    auto& getBuffer()
    {
        return buffer;
    }

    auto& getFifo()
    {
        return buffer;
    }

private:
    atk::FifoBuffer buffer;

    std::vector<float> tempReadBuffer;

    std::mutex writeLock;
    std::mutex readLock;
};

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
        std::lock_guard<std::mutex> lock1(writeLock);
        std::lock_guard<std::mutex> lock2(readLock);

        readerBufferSize = -1;
        writerBufferSize = -1;
        isPrepared = false;
    }

    void prepare()
    {
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

        fifoBuffer.setSize(numChannels, FIXED_BUFFER_SIZE);

        tempBuffer.resize(numChannels);
        for (auto& channel : tempBuffer)
            channel.resize(FIXED_BUFFER_SIZE);

        tempPtrs.resize(numChannels);

        readCallCount = 0;
        historyIndex = 0;
        bufferLevelHistory.assign(BUFFER_HISTORY_SIZE, readerBufferSize);

        bufferCompensation = 0.0;
        wasAtTargetLevel = false;

        isPrepared.store(true, std::memory_order_release);
    }

    int write(const float* const* src, int numChannels, int numSamples, double sampleRate)
    {
        std::unique_lock<std::mutex> lock(writeLock, std::try_to_lock);
        if (!lock.owns_lock())
            return 0;

        writerSampleRate = sampleRate;
        writerBufferSize = numSamples;

        if (writerNumChannels < numChannels)
        {
            writerNumChannels = numChannels;
            isPrepared.store(false, std::memory_order_release);
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        return fifoBuffer.write(src, numChannels, numSamples);
    }

    bool read(float* const* dest, int numChannels, int numSamples, double sampleRate, bool addToBuffer = false)
    {
        std::unique_lock<std::mutex> lock(readLock, std::try_to_lock);
        if (!lock.owns_lock())
            return false;

        readerSampleRate = sampleRate;
        readerBufferSize = numSamples > readerBufferSize ? numSamples : readerBufferSize;

        if (readerNumChannels < numChannels)
        {
            readerNumChannels = numChannels;
            isPrepared.store(false, std::memory_order_release);
        }

        if (!isPrepared.load(std::memory_order_acquire) //
            && writerNumChannels > 0
            && writerSampleRate > 0.0
            && writerBufferSize > 0)
        {
            // Need to prepare - must acquire writeLock
            lock.unlock();
            std::lock_guard<std::mutex> lock1(writeLock);
            std::lock_guard<std::mutex> lock2(readLock);
            prepare();
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return false;

        auto ratio = writerSampleRate / readerSampleRate;

        int samplesInFifo = fifoBuffer.getFifo().getNumReady();

        bufferLevelHistory[historyIndex] = samplesInFifo;
        readCallCount++;
        historyIndex = (historyIndex + 1) % BUFFER_HISTORY_SIZE;

        if (readCallCount >= BUFFER_HISTORY_SIZE)
        {
            int minBufferLevel = samplesInFifo;
            int maxBaseLevel = 0;
            for (int i = 0; i < BUFFER_HISTORY_SIZE; ++i)
            {
                minBufferLevel = std::min(minBufferLevel, bufferLevelHistory[i]);
                maxBaseLevel = std::max(maxBaseLevel, bufferLevelHistory[i]);
            }

            int baseTargetLevel = static_cast<int>(std::ceil(readerBufferSize * ratio));
            baseTargetLevel = baseTargetLevel > maxBaseLevel ? maxBaseLevel : baseTargetLevel;
            int targetMinLevel = static_cast<int>(std::ceil(baseTargetLevel * TARGET_LEVEL_FACTOR));
            int bufferLevelError = minBufferLevel - targetMinLevel;

            if (bufferLevelError != 0)
            {
                int64_t samplesReadInWindow = static_cast<int64_t>(readerBufferSize) * BUFFER_HISTORY_SIZE;
                bufferCompensation = static_cast<double>(bufferLevelError) / samplesReadInWindow;
            }
            else
            {
                bufferCompensation = 0.0;
            }

            bool currentlyAtTarget = (minBufferLevel >= targetMinLevel);
            if (currentlyAtTarget && !wasAtTargetLevel)
            {
#ifdef ATK_DEBUG
                auto now = std::chrono::system_clock::now();
                auto time_t_now = std::chrono::system_clock::to_time_t(now);
                std::ostringstream oss;
                oss << std::put_time(std::localtime(&time_t_now), "%H:%M:%S");
                auto timestamp = oss.str();
                DBG("[SYNC] "
                    << timestamp
                    << " Buffer level RECOVERED - min = "
                    << minBufferLevel
                    << ", target = "
                    << targetMinLevel);
#endif
            }
            wasAtTargetLevel = currentlyAtTarget;
        }

        auto compensatedRatio = ratio * (1.0 + bufferCompensation);
        int writerSamplesNeeded = static_cast<int>(std::ceil(numSamples * compensatedRatio)) + 1;

        if (!addToBuffer)
            for (int ch = 0; ch < numChannels; ++ch)
                std::memset(dest[ch], 0, sizeof(float) * numSamples);

        if (tempBuffer.size() < static_cast<size_t>(writerNumChannels))
            tempBuffer.resize(writerNumChannels);
        if (tempBuffer.size() > 0 && tempBuffer[0].size() < static_cast<size_t>(writerSamplesNeeded))
            for (auto& channel : tempBuffer)
                channel.resize(writerSamplesNeeded);

        if (tempPtrs.size() < static_cast<size_t>(writerNumChannels))
            tempPtrs.resize(writerNumChannels);

        for (int ch = 0; ch < writerNumChannels; ++ch)
            tempPtrs[ch] = tempBuffer[ch].data();

        auto writerSamples = fifoBuffer.read(tempPtrs.data(), writerNumChannels, writerSamplesNeeded, false);

        if (writerSamples == 0)
            return false;

        auto finalRatio = compensatedRatio;
        if (writerSamples < writerSamplesNeeded)
        {
            double availabilityFactor = static_cast<double>(writerSamples) / writerSamplesNeeded;
            finalRatio = compensatedRatio * availabilityFactor;

#ifdef ATK_DEBUG
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            std::ostringstream oss;
            oss << std::put_time(std::localtime(&time_t_now), "%H:%M:%S");
            auto timestamp = oss.str();
            DBG("[SYNC] "
                << timestamp
                << " UNDERFLOW - need "
                << writerSamplesNeeded
                << " samples, only got "
                << writerSamples
                << " - reduced ratio from "
                << compensatedRatio
                << " to "
                << finalRatio);
#endif
        }
        int maxSamplesConsumed = 0;

        auto channelGain = 1.0f;
        if (writerNumChannels > numChannels)
            channelGain = static_cast<float>(std::sqrt(static_cast<double>(numChannels) / writerNumChannels));

        for (int srcCh = 0; srcCh < writerNumChannels; ++srcCh)
        {
            int destCh = srcCh % numChannels;

            int samplesConsumed;
            if (srcCh < numChannels)
            {
                // First pass: write directly to destination
                samplesConsumed =
                    interpolators[srcCh]
                        .process(finalRatio, tempBuffer[srcCh].data(), dest[destCh], numSamples, writerSamples, 0);

                if (channelGain != 1.0f)
                    for (int j = 0; j < numSamples; ++j)
                        dest[destCh][j] *= channelGain;
            }
            else
            {
                // Subsequent passes: add to destination
                samplesConsumed = interpolators[srcCh].processAdding(
                    finalRatio,
                    tempBuffer[srcCh].data(),
                    dest[destCh],
                    numSamples,
                    writerSamples,
                    0,
                    channelGain
                );
            }

            maxSamplesConsumed = std::max(maxSamplesConsumed, samplesConsumed);
        }

        int samplesToAdvance = std::min(maxSamplesConsumed, writerSamples);

        if (maxSamplesConsumed > writerSamples)
        {
#ifdef ATK_DEBUG
            DBG("[SYNC] INFO - Interpolator consumed "
                << maxSamplesConsumed
                << " but only "
                << writerSamples
                << " available - advancing by "
                << samplesToAdvance);
#endif
        }

        fifoBuffer.advanceRead(samplesToAdvance);

        return true;
    }

private:
    std::atomic_bool isPrepared{false};
    int numChannels{0};

    FifoBuffer2 fifoBuffer;
    std::vector<atk::LagrangeInterpolator> interpolators;

    std::vector<std::vector<float>> tempBuffer;
    std::vector<float*> tempPtrs;

    int readerBufferSize{0};
    int writerBufferSize{0};
    int readerNumChannels{0};
    int writerNumChannels{0};

    double readerSampleRate{0.0};
    double writerSampleRate{0.0};

    static constexpr int BUFFER_HISTORY_SIZE = 1024;

    std::vector<int> bufferLevelHistory;
    int historyIndex{0};
    int readCallCount{0};

    double bufferCompensation{0.0};
    bool wasAtTargetLevel{false};

    std::mutex readLock;
    std::mutex writeLock;
};
