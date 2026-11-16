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

    // Largest output count whose required input fits in numInputSamples.
    int getAvailableOutputSamples(double speedRatio, int numInputSamples)
    {
        if (numInputSamples < static_cast<int>(subSamplePos))
            return 0;
        int count = static_cast<int>(std::ceil((numInputSamples + 1 - subSamplePos) / speedRatio));
        // The closed form can be one off after rounding; settle it against the exact requirement.
        while (count > 0 && getRequiredInputSamples(speedRatio, count) > numInputSamples)
            --count;
        while (getRequiredInputSamples(speedRatio, count + 1) <= numInputSamples)
            ++count;
        return count;
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
        fifo.setSize(numChannels, capacity);
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

namespace atk
{
// Smooth position of a frame stream on the shared monotonic clock: originSeconds is when frame 0 occurred.
struct StreamTimeline
{
    double originSeconds{0.0};
    bool isValid{false};
};

// Follows a real-time producer's delivery of everything up to stream position `frames` at timeSeconds and
// returns how late it was against the timeline. A producer cannot deliver audio before producing it, so an
// early delivery means the timeline is late and it moves there at once; so does a delivery later than
// resyncSeconds (a stall). Other late deliveries steer it at a narrow bandwidth.
double TrackTimeline(
    StreamTimeline* timeline,
    int64_t frames,
    double timeSeconds,
    double rate,
    double intervalSeconds,
    double resyncSeconds
);

// Peak of a value that rises immediately; values within 10% of the last raise renew it. After holdSeconds
// without renewal it falls at releasePerSecond.
struct PeakHold
{
    double value{0.0};
    double raisedValue{0.0};
    double peakSeconds{0.0};
    double lastSeconds{0.0};
};

void HoldPeak(PeakHold* peak, double value, double timeSeconds, double holdSeconds, double releasePerSecond);
} // namespace atk

// Drift-correcting bridge between two clock domains on one monotonic clock (juce::Time hi-res counter).
//
//   write(n, t): writer timeline -> lateness = source frames produced since the previous delivery
//                writer reserve  = held peak lateness (beyond maxLatenessSeconds counts as a stall)
//   read(m, t):  level  = buffered + produced-but-undelivered (smooth: no block sawtooth, no callback phase)
//                demand = input needed now + dip of the level below its recent mean (an early read)
//                reader reserve = held peak demand
//                target = reader reserve + writer reserve + safety
//                PI on level - target, clamped; gross excess is dropped with a fade
//   Shortages render what is buffered with a fade-out and hold the rest; the late delivery raises the
//   writer reserve by the time the reader waited, so the controller never mistakes it for drift.
//
// Formats follow the streams: a write or read with new channels or a new rate restarts the stream within the
// preallocated storage, and a larger read widens the reader's block without a restart.
class SyncBuffer
{
public:
    struct StorageLimits
    {
        int channels{16};
        int fifoCapacity{65536};
        int maxWriteFrames{16384};
    };

    struct Tuning
    {
        double correctionTimeSeconds{3.0};
        double errorFilterSeconds{1.0};
        double maxLatenessSeconds{0.1};
        // A reserve peak is kept this long after its last recurrence, then released at this fraction of the
        // rate per second (500 ppm is under a cent of pitch while the controller follows it).
        double reserveHoldSeconds{60.0};
        double reserveReleaseRate{500.0e-6};
        double safetySeconds{0.0005};
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
        double levelFrames{0.0};
        double targetFrames{0.0};
        double writerReserveFrames{0.0};
        double readerReserveFrames{0.0};
        double errorFrames{0.0};
        double filteredErrorFrames{0.0};
        double correction{0.0};
        double driftCorrection{0.0};
        uint64_t underruns{0};
        uint64_t overflows{0};
        uint64_t recoveries{0};
        uint64_t drops{0};
        uint64_t staleReaderResets{0};
        uint64_t writerStalls{0};
    };

    explicit SyncBuffer(const juce::String& debugTag = "");
    SyncBuffer(StorageLimits limits, const juce::String& debugTag = "");
    ~SyncBuffer();
    void setTag(const juce::String& debugTag);
    // Preparation, tuning, and lifecycle changes run outside audio callbacks.
    // Expects a format so the first writes are kept; storage grows to fit it.
    bool prepare(int numChannels, int bufferSize, double sampleRate);
    bool setTuning(Tuning next);
    void clearPrepared();
    void reset();
    bool getIsPrepared() const;
    int getNumReady() const;
    int getTargetLevel() const;
    double getBufferCompensation() const;
    // Diagnostics are read outside audio callbacks.
    Diagnostics getDiagnostics();

    // Returns all requested frames or zero; unread input is never overwritten.
    // Timestamps must come from the clock read() uses; the overloads without one read juce::Time.
    int write(const float* const* src, int numChannels, int numSamples, double sampleRate);
    int write(const float* const* src, int numChannels, int numSamples, double sampleRate, double timeSeconds);
    // Always fills the output; returns whether it contains audio. A stalled reader discards its backlog.
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

    void allocateStorage();
    void restartStream();
    void restartReader();
    void trackWrite(int frames, double timeSeconds);
    void trackRead(int need, double level, double timeSeconds);
    double getUndelivered(int available, double timeSeconds);
    void updateCorrection(double errorFrames, double seconds, bool learnDrift);
    void publishStats();
    bool formatsFit(StreamFormat input, StreamFormat output);
    bool render(float* const* dest, int numChannels, int numSamples, double timeSeconds);
    int renderFrames(float* const* dest, int numChannels, int numSamples, double ratio, int inputFrames);

    Tuning tuning;
    StorageLimits storage;
    StreamFormat writer;
    StreamFormat reader;
    std::shared_ptr<StatsLogger> statsLogger;
    FifoBuffer2 fifo;
    std::vector<atk::Interpolator> interpolators;
    juce::AudioBuffer<float> scratch;

    // Writer side, owned under writeLock and published to the reader through atomics.
    atk::StreamTimeline writerTimeline;
    atk::PeakHold writerLateness;
    int64_t writtenFrames{0};
    std::atomic<double> writerOrigin{0.0};
    std::atomic<double> writerReserve{0.0};
    std::atomic_bool hasWriterTimeline{false};
    std::atomic_bool hasWriterReserve{false};
    std::atomic<uint64_t> overflows{0};
    std::atomic<uint64_t> writerStalls{0};

    // Reader side, owned under readLock.
    Diagnostics diagnostics;
    atk::PeakHold readerDemand;
    double meanLevel{0.0};
    double previousTarget{0.0};
    double lastTargetFallSeconds{0.0};
    int64_t consumedFrames{0};
    int fadePosition{0};
    bool hasReadTime{false};
    double firstReadTime{0.0};
    double lastReadTime{0.0};

    std::atomic_bool isPrepared{false};
    std::atomic<int> targetLevel{0};
    std::atomic<double> compensation{0.0};
    std::mutex readLock;
    std::mutex writeLock;
};
