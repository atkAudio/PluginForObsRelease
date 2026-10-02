#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>
#include "atkaudio.h"
#include "FifoBuffer.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace atk
{

inline int nextPowerOfTwo(int value) noexcept
{
    int result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

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
        int consumed = 0;
        for (int i = 0; i < numOutputSamples; ++i)
        {
            while (subSamplePos >= 1.0)
            {
                if (consumed >= numInputSamples)
                    return consumed;

                history[oldestIndex] = input[consumed++];
                oldestIndex = (oldestIndex + 1) % HISTORY_SIZE;
                subSamplePos -= 1.0;
            }

            output[i] += gain * interpolate();
            subSamplePos += speedRatio;
        }
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

// Measures a stream's sample rate against the host clock over a moving window of recent entries,
// between the averages of its older and newer halves so single-callback jitter averages out.
class StreamClock
{
public:
    // Entries are spaced by entrySamples, so streams with different block sizes span the same time.
    void reset(double sampleRate, int blockSize, int entrySamples) noexcept
    {
        nominalRate = sampleRate;
        stallSeconds = STALL_BLOCKS * blockSize / sampleRate;
        samplesPerEntry = entrySamples;
        restart();
    }

    void restart() noexcept
    {
        numBlocks = 0;
        numEntries = 0;
        totalSamples = 0;
        nextEntrySamples = 0;
        older = {};
        newer = {};
    }

    // Returns true when a stall restarted the count.
    bool addBlock(int numSamples, double timeSeconds) noexcept
    {
        const double gap = timeSeconds - lastTime;
        lastTime = timeSeconds;

        const bool isStall = numBlocks > 0 && gap > stallSeconds;
        if (isStall)
            restart();
        // A catch-up burst lands all at once; start counting from its last block.
        if (numBlocks == 1 && gap < 0.5 * numSamples / nominalRate)
            restart();

        if (numBlocks == 0)
            startTime = timeSeconds;
        if (totalSamples >= nextEntrySamples)
        {
            addEntry({timeSeconds - startTime, static_cast<double>(totalSamples)});
            nextEntrySamples = totalSamples + samplesPerEntry;
        }

        totalSamples += numSamples;
        ++numBlocks;
        return isStall;
    }

    bool isPrimed() const noexcept
    {
        return numEntries >= WINDOW_ENTRIES && newer.time > older.time;
    }

    // Measured over nominal rate.
    double getRateFactor() const noexcept
    {
        return (newer.samples - older.samples) / (newer.time - older.time) / nominalRate;
    }

private:
    struct Entry
    {
        double time{0.0};
        double samples{0.0};
    };

    void addEntry(const Entry& entry) noexcept
    {
        if (numEntries >= WINDOW_ENTRIES)
        {
            const auto& leaving = window[numEntries % WINDOW_ENTRIES];
            older.time -= leaving.time;
            older.samples -= leaving.samples;
        }
        if (numEntries >= HALF_ENTRIES)
        {
            const auto& crossing = window[(numEntries - HALF_ENTRIES) % WINDOW_ENTRIES];
            newer.time -= crossing.time;
            newer.samples -= crossing.samples;
            older.time += crossing.time;
            older.samples += crossing.samples;
        }
        window[numEntries % WINDOW_ENTRIES] = entry;
        newer.time += entry.time;
        newer.samples += entry.samples;
        ++numEntries;
    }

    static constexpr int WINDOW_ENTRIES = 1024;
    static constexpr int HALF_ENTRIES = WINDOW_ENTRIES / 2;
    static constexpr double STALL_BLOCKS = 4.0;

    double nominalRate{1.0};
    double stallSeconds{0.0};
    int samplesPerEntry{1};
    int64_t numBlocks{0};
    int64_t numEntries{0};
    int64_t totalSamples{0};
    int64_t nextEntrySamples{0};
    double startTime{0.0};
    double lastTime{0.0};
    std::array<Entry, WINDOW_ENTRIES> window{};
    Entry older;
    Entry newer;
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
    }

    void reset()
    {
        fifo.reset();
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
};

// Clock-drift absorber. Drift measured by counting samples against host time is fed forward, and a slow
// proportional trim holds the level at a target of one read plus interpolation and drift headroom.
class SyncBuffer
{
public:
    static constexpr double MAX_DRIFT = 300.0e-6;
    // Drift feed-forward plus a trim that alone can hold the worst-case drift.
    static constexpr double CORRECTION_AUTHORITY = 2.0 * MAX_DRIFT;

    explicit SyncBuffer(const juce::String& debugTag = "")
        : tag(debugTag)
    {
    }

    void setTag(const juce::String& debugTag)
    {
        tag = debugTag;
    }

    int getNumReady() const
    {
        return fifo.getNumReady();
    }

    bool getIsPrepared() const
    {
        return isPrepared.load(std::memory_order_acquire);
    }

    void prepare(int numChannels, int bufferSize, double sampleRate)
    {
        std::scoped_lock lock(writeLock, readLock);
        reader = writer = StreamFormat{numChannels, bufferSize, sampleRate};
        prepareLocked();
    }

    void clearPrepared()
    {
        std::scoped_lock lock(writeLock, readLock);
        reader.blockSize = 0;
        writer.blockSize = 0;
        isPrepared.store(false, std::memory_order_release);
    }

    void reset()
    {
        std::scoped_lock lock(writeLock, readLock);
        fifo.reset();
        writerClock.restart();
        readerClock.restart();
        resetDriftState();
    }

    int write(const float* const* src, int numChannels, int numSamples, double sampleRate)
    {
        return write(src, numChannels, numSamples, sampleRate, getHostTimeSeconds());
    }

    int write(const float* const* src, int numChannels, int numSamples, double sampleRate, double timeSeconds)
    {
        std::unique_lock lock(writeLock, std::try_to_lock);
        if (!lock.owns_lock())
        {
            restartWriterClock.store(true, std::memory_order_relaxed);
            logDrop("writer lock contended", numSamples);
            return 0;
        }

        if (writer.update(numChannels, numSamples, sampleRate))
            isPrepared.store(false, std::memory_order_release);

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        if (restartWriterClock.exchange(false, std::memory_order_relaxed))
            writerClock.restart();
        if (writerClock.addBlock(numSamples, timeSeconds))
            logEvent("writer stalled, drift measurement restarted");
        if (writerClock.isPrimed())
            writerRateFactor.store(writerClock.getRateFactor(), std::memory_order_relaxed);
        isWriterClockPrimed.store(writerClock.isPrimed(), std::memory_order_release);

        const int written = fifo.write(src, numChannels, numSamples);
        if (written > 0)
        {
            lastWriteSamples.store(written, std::memory_order_relaxed);
            lastWriteTime.store(timeSeconds, std::memory_order_release);
        }

        if (written < numSamples)
        {
            if (overflowDroppedSamples == 0)
                logDrop("writer overflow started", numSamples - written);
            overflowDroppedSamples += numSamples - written;
        }
        else if (overflowDroppedSamples > 0)
        {
            logDrop("writer overflow ended, total", overflowDroppedSamples);
            overflowDroppedSamples = 0;
        }
        return written;
    }

    // Always overwrites dest; returns false (with silence) while priming, on underflow or contention.
    bool read(float* const* dest, int numChannels, int numSamples, double sampleRate)
    {
        return read(dest, numChannels, numSamples, sampleRate, getHostTimeSeconds());
    }

    bool read(float* const* dest, int numChannels, int numSamples, double sampleRate, double timeSeconds)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            std::fill_n(dest[ch], numSamples, 0.0f);

        std::unique_lock lock(readLock, std::try_to_lock);
        if (!lock.owns_lock())
        {
            restartReaderClock.store(true, std::memory_order_relaxed);
            logDrop("reader lock contended", numSamples);
            return false;
        }

        if (reader.update(numChannels, numSamples, sampleRate))
            isPrepared.store(false, std::memory_order_release);

        if (!isPrepared.load(std::memory_order_acquire) && writer.isValid())
        {
            // Re-prepare needs both locks; skip this block rather than block the audio thread.
            lock.unlock();
            std::unique_lock writerLock(writeLock, std::try_to_lock);
            if (!writerLock.owns_lock())
                return false;
            lock = std::unique_lock(readLock, std::try_to_lock);
            if (!lock.owns_lock())
                return false;
            prepareLocked();
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return false;

        if (restartReaderClock.exchange(false, std::memory_order_relaxed))
            readerClock.restart();
        if (readerClock.addBlock(numSamples, timeSeconds))
            logEvent("reader stalled, drift measurement restarted");

        // Loaded before the level, so a racing write can only overstate it for one read.
        const double lastWrite = lastWriteTime.load(std::memory_order_acquire);
        const double lastBlock = lastWriteSamples.load(std::memory_order_relaxed);
        int level = fifo.getNumReady();
        // Counting the last block only as far as the writer clock has produced it removes the block ripple.
        const double producedOfLastBlock = std::clamp((timeSeconds - lastWrite) * writer.sampleRate, 0.0, lastBlock);
        double continuousLevel = level - (lastBlock - producedOfLastBlock);
        const int targetLevel = getTargetLevel();

        if (isPriming)
        {
            if (continuousLevel < targetLevel)
                return false;
            // Output is still silent, so a startup burst can be dropped here without a glitch.
            const int excess = static_cast<int>(continuousLevel - targetLevel);
            fifo.advanceRead(excess);
            level -= excess;
            continuousLevel -= excess;
            isPriming = false;
            logState("PRIMED", level, targetLevel);
        }

        updateCompensation(continuousLevel, targetLevel, numSamples);

        const double ratio = writer.sampleRate / reader.sampleRate * (1.0 + bufferCompensation);
        const int needed = static_cast<int>(std::ceil(numSamples * ratio)) + 1;

        if (level < needed)
        {
            logState("UNDERFLOW", level, targetLevel);
            readerClock.restart();
            restartWriterClock.store(true, std::memory_order_relaxed);
            startPriming();
            return false;
        }

        const int numSourceChannels = static_cast<int>(interpolators.size());
        scratch.setSize(numSourceChannels, needed, false, false, true);
        fifo.peek(scratch.getArrayOfWritePointers(), numSourceChannels, needed);

        // Surplus writer channels fold onto reader channels at reduced gain.
        const float foldGain = numSourceChannels > numChannels
                                 ? std::sqrt(static_cast<float>(numChannels) / static_cast<float>(numSourceChannels))
                                 : 1.0f;

        int consumed = 0;
        for (int ch = 0; ch < numSourceChannels; ++ch)
        {
            const float gain = ch < numChannels ? 1.0f : foldGain;
            const int channelConsumed =
                interpolators[ch]
                    .processAdding(ratio, scratch.getReadPointer(ch), dest[ch % numChannels], numSamples, needed, gain);
            consumed = std::max(consumed, channelConsumed);
        }
        fifo.advanceRead(consumed);

        return true;
    }

    double getBufferCompensation()
    {
        std::lock_guard lock(readLock);
        return bufferCompensation;
    }

    double getDriftEstimate()
    {
        std::lock_guard lock(readLock);
        return driftEstimate;
    }

    // The trim saturates at its authority when the level is this far below target.
    int getTargetLevel() const
    {
        return readBlockInWriterSamples()
             + INTERPOLATOR_MARGIN
             + static_cast<int>(std::ceil(getTrimAuthority() * getTrimHorizon()));
    }

private:
    struct StreamFormat
    {
        int numChannels{0};
        int blockSize{0};
        double sampleRate{0.0};

        // Keeps the largest channel count and block size seen; returns true if a re-prepare is needed.
        bool update(int channels, int samples, double rate) noexcept
        {
            if (channels <= numChannels && samples <= blockSize && rate == sampleRate)
                return false;
            numChannels = std::max(numChannels, channels);
            blockSize = std::max(blockSize, samples);
            sampleRate = rate;
            return true;
        }

        bool isValid() const noexcept
        {
            return numChannels > 0 && blockSize > 0 && sampleRate > 0.0;
        }
    };

    int readBlockInWriterSamples() const
    {
        return static_cast<int>(std::ceil(reader.blockSize * writer.sampleRate / reader.sampleRate));
    }

    int getLargerBlock() const
    {
        return std::max(writer.blockSize, readBlockInWriterSamples());
    }

    int getLargerBlockInReaderSamples() const
    {
        return static_cast<int>(std::ceil(getLargerBlock() * reader.sampleRate / writer.sampleRate));
    }

    double getTrimHorizon() const
    {
        return TRIM_HORIZON_BLOCKS * getLargerBlock();
    }

    // Unmeasured, the trim alone must hold the worst drift; measured, only the estimate's error.
    double getTrimAuthority() const
    {
        return isDriftMeasured ? MAX_RESIDUAL_DRIFT : MAX_DRIFT;
    }

    static double getHostTimeSeconds() noexcept
    {
        return juce::Time::getMillisecondCounterHiRes() * 0.001;
    }

    void prepareLocked()
    {
        isPrepared.store(false, std::memory_order_release);

        if (!reader.isValid() || !writer.isValid())
            return;

        interpolators.assign(writer.numChannels, atk::Interpolator{});

        writerClock.reset(writer.sampleRate, writer.blockSize, getLargerBlock());
        readerClock.reset(reader.sampleRate, reader.blockSize, getLargerBlockInReaderSamples());
        restartWriterClock.store(false, std::memory_order_relaxed);
        restartReaderClock.store(false, std::memory_order_relaxed);
        isWriterClockPrimed.store(false, std::memory_order_relaxed);
        lastWriteSamples.store(0, std::memory_order_relaxed);
        lastWriteTime.store(0.0, std::memory_order_relaxed);
        driftEstimate = 0.0;
        isDriftMeasured = false;

        const int capacity =
            atk::nextPowerOfTwo(std::max(2 * (getTargetLevel() + writer.blockSize), MIN_FIFO_CAPACITY));
        fifo.setSize(writer.numChannels, capacity);
        scratch.setSize(writer.numChannels, capacity);

        resetDriftState();

        isPrepared.store(true, std::memory_order_release);
    }

    // The last drift estimate is kept until the clocks prime again: it describes the hardware, not the FIFO.
    void resetDriftState()
    {
        startPriming();
        windowReaderSamples = 0;
        bufferCompensation = driftEstimate;
    }

    void startPriming()
    {
        isPriming = true;
        for (auto& interpolator : interpolators)
            interpolator.reset();
    }

    void updateCompensation(double level, int targetLevel, int numSamples)
    {
        if (windowReaderSamples == 0 || level < windowMinLevel)
            windowMinLevel = level;
        windowReaderSamples += numSamples;

        if (windowReaderSamples < WINDOW_BLOCKS * getLargerBlockInReaderSamples())
            return;

        const bool wasDriftMeasured = isDriftMeasured;
        isDriftMeasured = readerClock.isPrimed() && isWriterClockPrimed.load(std::memory_order_acquire);
        const int loggedLevel = static_cast<int>(std::lround(windowMinLevel));
        if (isDriftMeasured)
        {
            const double measured =
                writerRateFactor.load(std::memory_order_relaxed) / readerClock.getRateFactor() - 1.0;
            driftEstimate = std::clamp(measured, -MAX_DRIFT, MAX_DRIFT);

            if (!wasDriftMeasured || std::abs(driftEstimate - loggedDriftEstimate) > DRIFT_LOG_STEP)
            {
                loggedDriftEstimate = driftEstimate;
                logState(wasDriftMeasured ? "DRIFT CHANGED" : "DRIFT MEASURED", loggedLevel, targetLevel);
            }
        }
        else if (wasDriftMeasured)
        {
            logState("DRIFT UNMEASURED", loggedLevel, targetLevel);
        }

        const double authority = getTrimAuthority();
        const double trim = std::clamp((windowMinLevel - targetLevel) / getTrimHorizon(), -authority, authority);
        bufferCompensation = driftEstimate + trim;

        windowReaderSamples = 0;
    }

    void
    logState([[maybe_unused]] const char* event, [[maybe_unused]] int level, [[maybe_unused]] int targetLevel) const
    {
        ATK_DBG(
            logPrefix()
            << event
            << " level="
            << level
            << " target="
            << targetLevel
            << " compensation="
            << bufferCompensation
            << " drift="
            << driftEstimate
        );
    }

    // Touches no reader/writer state, so it is safe from either thread and without locks.
    void logDrop([[maybe_unused]] const char* reason, [[maybe_unused]] int64_t droppedSamples) const
    {
        ATK_DBG(logPrefix() << "DROP " << reason << " samples=" << droppedSamples);
    }

    void logEvent([[maybe_unused]] const char* message) const
    {
        ATK_DBG(logPrefix() << message);
    }

#ifdef ATK_DEBUG
    std::string logPrefix() const
    {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::ostringstream prefix;
        prefix
            << "[SYNC] "
            << (tag.isNotEmpty() ? tag + " " : "")
            << std::put_time(std::localtime(&now), "%H:%M:%S")
            << " ";
        return prefix.str();
    }
#endif

    static constexpr int MIN_FIFO_CAPACITY = 4096;
    static constexpr int INTERPOLATOR_MARGIN = 4;
    // Periods count the larger of the two blocks, since that sets both the ripple and the scheduling beat.
    // A longer window gives a steadier minimum; 8 windows per trim horizon keeps the loop well damped.
    static constexpr int WINDOW_BLOCKS = 32;
    static constexpr int TRIM_HORIZON_BLOCKS = 256;
    // Worst expected error of a primed drift estimate.
    static constexpr double MAX_RESIDUAL_DRIFT = 30.0e-6;
    static constexpr double DRIFT_LOG_STEP = 10.0e-6;

    juce::String tag;
    std::atomic_bool isPrepared{false};

    StreamFormat reader;
    StreamFormat writer;

    FifoBuffer2 fifo;
    std::vector<atk::Interpolator> interpolators;
    juce::AudioBuffer<float> scratch;

    atk::StreamClock writerClock;
    atk::StreamClock readerClock;
    // A missed block would read as drift, so contention, underflow and stalls restart the count instead.
    std::atomic_bool restartWriterClock{false};
    std::atomic_bool restartReaderClock{false};
    std::atomic_bool isWriterClockPrimed{false};
    std::atomic<double> writerRateFactor{1.0};
    std::atomic<int> lastWriteSamples{0};
    std::atomic<double> lastWriteTime{0.0};

    bool isPriming{true};
    bool isDriftMeasured{false};
    int64_t overflowDroppedSamples{0};
    double windowMinLevel{0.0};
    int windowReaderSamples{0};
    double bufferCompensation{0.0};
    double driftEstimate{0.0};
    double loggedDriftEstimate{0.0};

    std::mutex readLock;
    std::mutex writeLock;
};
