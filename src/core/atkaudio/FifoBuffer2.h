#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "atkaudio.h"
#include "FifoBuffer.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace atk
{

// Streaming 5-point Lagrange resampler.
class Interpolator
{
public:
    void reset() noexcept
    {
        std::fill(std::begin(history), std::end(history), 0.0f);
        oldestIndex = 0;
        subSamplePos = 1.0;
    }

    int getRequiredInputSamples(double speedRatio, int numOutputSamples)
    {
        if (numOutputSamples <= 0)
            return 0;
        return static_cast<int>(subSamplePos + (numOutputSamples - 1) * speedRatio);
    }

    double getInputPosition()
    {
        return subSamplePos;
    }

    // Adds gain * resampled input to output; returns the number of input samples consumed.
    int processAdding(
        double speedRatio,
        const float* input,
        float* output,
        int numOutputSamples,
        int numInputSamples,
        float gain
    ) noexcept
    {
        double startPosition = subSamplePos;
        int consumed = 0;
        for (int i = 0; i < numOutputSamples; ++i)
        {
            double position = startPosition + i * speedRatio;
            int required = static_cast<int>(position);
            while (consumed < required)
            {
                if (consumed >= numInputSamples)
                {
                    subSamplePos = position - consumed;
                    return consumed;
                }

                history[oldestIndex] = input[consumed++];
                oldestIndex = (oldestIndex + 1) % HISTORY_SIZE;
            }

            subSamplePos = position - consumed;
            output[i] += gain * interpolate();
        }
        subSamplePos = startPosition + numOutputSamples * speedRatio - consumed;
        return consumed;
    }

private:
    float sampleAt(int age) const noexcept
    {
        return history[(oldestIndex + age) % HISTORY_SIZE];
    }

    template <int k>
    static float lagrangeTerm(float input, float offset) noexcept
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
        const auto offset = static_cast<float>(subSamplePos);
        return lagrangeTerm<0>(sampleAt(0), offset)
             + lagrangeTerm<1>(sampleAt(1), offset)
             + lagrangeTerm<2>(sampleAt(2), offset)
             + lagrangeTerm<3>(sampleAt(3), offset)
             + lagrangeTerm<4>(sampleAt(4), offset);
    }

    static constexpr int HISTORY_SIZE = 5;

    float history[HISTORY_SIZE]{};
    int oldestIndex{0};
    double subSamplePos{1.0};
};

} // namespace atk

// Multichannel SPSC FIFO; setSize/reset require both sides idle.
class FifoBuffer2
{
public:
    void setSize(int numChannels, int capacity)
    {
        // +1: the ring keeps one slot free to tell full from empty.
        fifo.setSize(numChannels, capacity + 1);
        silence.assign(capacity, 0.0f);
    }

    void reset()
    {
        fifo.resetPositions();
    }

    int getNumReady() const
    {
        return fifo.getNumReady();
    }

    int write(const float* const* src, int numChannels, int numSamples)
    {
        numChannels = std::min(numChannels, fifo.getNumChannels());
        const int toWrite = std::min(numSamples, fifo.getFreeSpace());
        if (toWrite <= 0)
            return 0;

        for (int ch = 0; ch < numChannels; ++ch)
            fifo.write(src[ch], ch, toWrite, ch == numChannels - 1);
        return toWrite;
    }

    bool tryWrite(const float* const* src, int numChannels, int numSamples)
    {
        if (numSamples <= 0
            || numChannels <= 0
            || numChannels > fifo.getNumChannels()
            || numSamples > fifo.getFreeSpace())
            return false;
        for (int ch = 0; ch < fifo.getNumChannels(); ++ch)
            fifo.write(ch < numChannels ? src[ch] : silence.data(), ch, numSamples, ch == fifo.getNumChannels() - 1);
        return true;
    }

    int peek(float* const* dest, int numChannels, int numSamples)
    {
        numChannels = std::min(numChannels, fifo.getNumChannels());
        const int toRead = std::min(numSamples, fifo.getNumReady());
        if (toRead <= 0)
            return 0;

        for (int ch = 0; ch < numChannels; ++ch)
            fifo.read(dest[ch], ch, toRead, false);
        return toRead;
    }

    int read(float* const* dest, int numChannels, int numSamples)
    {
        const int toRead = peek(dest, numChannels, numSamples);
        if (toRead > 0)
            advanceRead(toRead);
        return toRead;
    }

    void advanceRead(int numSamples)
    {
        fifo.advanceRead(numSamples);
    }

private:
    atk::FifoBuffer fifo;
    std::vector<float> silence;
};

class SyncBuffer
{
public:
    struct StorageLimits
    {
        int channels{16};
        int fifoCapacity{65536};
        int maxWriteFrames{16384};
    };

    struct Parameters
    {
        double inputRate{0.0};
        double outputRate{0.0};
        int channels{0};
        int maxReadFrames{0};
        int maxWriteFrames{0};
        int fifoCapacity{0};
        double correctionTimeSeconds{1.0};
        double correctionLimit{300.0e-6};
        double readerStallPeriods{4.0};
        int fadeFrames{32};
    };

    enum State
    {
        prefill,
        running,
        recovering
    };

    struct Diagnostics
    {
        State state{prefill};
        int callbackFrames{0};
        int availableFrames{0};
        int actualNeed{0};
        double levelErrorFrames{0.0};
        double latestTrough{0.0};
        bool hasTrough{false};
        double correction{0.0};
        int prefillFrames{0};
        uint64_t underruns{0};
        uint64_t overflows{0};
        uint64_t recoveries{0};
        uint64_t staleReaderResets{0};
    };

    explicit SyncBuffer(const juce::String& debugTag = "");
    SyncBuffer(StorageLimits limits, const juce::String& debugTag = "");
    ~SyncBuffer();
    void setTag(const juce::String& debugTag);
    // Preparation and lifecycle changes run outside audio callbacks. Parameters fix the stream format.
    bool prepare(Parameters parameters);
    // Legacy format discovery reuses the constructor's preallocated storage limits.
    void prepare(int numChannels, int bufferSize, double sampleRate);
    void clearPrepared();
    void reset();
    bool getIsPrepared() const;
    int getNumReady() const;
    int getTargetLevel() const;
    double getBufferCompensation() const;
    // Diagnostics are read outside audio callbacks.
    Diagnostics getDiagnostics();

    // Returns all requested frames or zero; unread input is never overwritten.
    int write(const float* const* src, int numChannels, int numSamples, double sampleRate);
    int write(const float* const* src, int numChannels, int numSamples, double sampleRate, double timeSeconds);
    // Always fills the output. Shortages hold input; a stalled reader discards its backlog on resuming.
    bool read(float* const* dest, int numChannels, int numSamples, double sampleRate);
    bool read(float* const* dest, int numChannels, int numSamples, double sampleRate, double timeSeconds);

private:
    struct StatsLogger;

    struct StreamFormat
    {
        int channels{0};
        int maxFrames{0};
        double rate{0.0};
    };

    void allocateStorage(int channels, int capacity, int scratchFrames);
    bool prepareStream(Parameters parameters, bool fixedFormat);
    void restartStream();
    void clearMeasurement();
    void updateMeasurement();
    void publishStats();
    bool formatsFit(StreamFormat input, StreamFormat output);
    bool render(float* const* dest, int numChannels, int numSamples);

    Parameters parameters;
    StorageLimits storageLimits;
    StreamFormat writer;
    StreamFormat reader;
    bool fixedFormat{false};
    std::shared_ptr<StatsLogger> statsLogger;
    FifoBuffer2 fifo;
    std::vector<atk::Interpolator> interpolators;
    juce::AudioBuffer<float> scratch;
    Diagnostics diagnostics;
    int previousLevel{0};
    bool hasPreviousLevel{false};
    bool wasFalling{false};
    int fadePosition{0};
    bool hasReadTime{false};
    double firstReadTime{0.0};
    double lastReadTime{0.0};
    std::atomic_bool isPrepared{false};
    std::atomic<int> targetLevel{0};
    std::atomic<double> compensation{0.0};
    std::atomic<uint64_t> overflows{0};
    std::mutex readLock;
    std::mutex writeLock;
};
