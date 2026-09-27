#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
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

// Streaming 5-point Lagrange resampler; at a snapped phase it passes samples through bit-exact.
class Interpolator
{
public:
    void reset() noexcept
    {
        std::fill(std::begin(history), std::end(history), 0.0f);
        oldestIndex = 0;
        subSamplePos = 1.0;
    }

    // Aligns to a whole sample without clearing history, so the output stays continuous.
    void snapPhase() noexcept
    {
        subSamplePos = 0.0;
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

// Clock-drift absorber. Drift runs free inside an accept zone above the safety floor; once the tracked
// level leaves it, a fixed correction eases in until the level is back at the zone centre.
class SyncBuffer
{
public:
    static constexpr double CORRECTION_AUTHORITY = 500.0e-6;

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
        resetDriftState();
    }

    int write(const float* const* src, int numChannels, int numSamples, double sampleRate)
    {
        std::unique_lock lock(writeLock, std::try_to_lock);
        if (!lock.owns_lock())
        {
            logDrop("writer lock contended", numSamples);
            return 0;
        }

        if (writer.update(numChannels, numSamples, sampleRate))
            isPrepared.store(false, std::memory_order_release);

        if (!isPrepared.load(std::memory_order_acquire))
            return 0;

        const int written = fifo.write(src, numChannels, numSamples);
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
        for (int ch = 0; ch < numChannels; ++ch)
            std::fill_n(dest[ch], numSamples, 0.0f);

        std::unique_lock lock(readLock, std::try_to_lock);
        if (!lock.owns_lock())
        {
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

        int level = fifo.getNumReady();
        const int targetLevel = getTargetLevel();

        if (isPriming)
        {
            // Priming fires right after a writer block lands; the level then sags by (W - R) before the next.
            const int writerSag = std::max(0, writer.blockSize - readBlockInWriterSamples());
            const int primedLevel = targetLevel + writerSag;
            if (level < primedLevel)
                return false;
            // Output is still silent, so a startup burst can be dropped here without a glitch.
            fifo.advanceRead(level - primedLevel);
            level = primedLevel;
            isPriming = false;
            logState("PRIMED", level, targetLevel);
        }

        // Must run before the ratio is read, so a phase snap and its matching ratio land on the same block.
        updateCompensation(level, targetLevel, numSamples);

        const double ratio = writer.sampleRate / reader.sampleRate * (1.0 + bufferCompensation);
        const int needed = static_cast<int>(std::ceil(numSamples * ratio)) + 1;

        if (level < needed)
        {
            logState("UNDERFLOW", level, targetLevel);
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

    bool getIsUnity()
    {
        std::lock_guard lock(readLock);
        return isUnity;
    }

    // Centre of the accept zone that drift is allowed to wander across.
    int getTargetLevel() const
    {
        return getZoneFloor() + getZoneWidth() / 2;
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

    // What one read needs, plus a writer block. windowMinLevel is only the trough already seen: when
    // the write/read interleaving slips by one the level drops a whole writer block with no warning,
    // so that block has to be sitting there in advance.
    int getZoneFloor() const
    {
        return readBlockInWriterSamples() + INTERPOLATOR_MARGIN + writer.blockSize;
    }

    int getZoneWidth() const
    {
        return readBlockInWriterSamples() / 2;
    }

    void prepareLocked()
    {
        isPrepared.store(false, std::memory_order_release);

        if (!reader.isValid() || !writer.isValid())
            return;

        interpolators.assign(writer.numChannels, atk::Interpolator{});

        const int capacity =
            atk::nextPowerOfTwo(std::max(2 * (getTargetLevel() + writer.blockSize), MIN_FIFO_CAPACITY));
        fifo.setSize(writer.numChannels, capacity);
        scratch.setSize(writer.numChannels, capacity);

        resetDriftState();

        isPrepared.store(true, std::memory_order_release);
    }

    void resetDriftState()
    {
        startPriming();
        isUnity = false;
        correctionDirection = 0;
        trackedLevel = getTargetLevel();
        windowMinLevel = INT_MAX;
        windowReaderSamples = 0;
        bufferCompensation = 0.0;
    }

    void startPriming()
    {
        isPriming = true;
        for (auto& interpolator : interpolators)
            interpolator.reset();
    }

    // Thermostat with a fixed correction rather than an accumulating one, so nothing can wind up.
    void updateCompensation(int level, int targetLevel, int numSamples)
    {
        windowMinLevel = std::min(windowMinLevel, level);
        windowReaderSamples += numSamples;

        if (windowReaderSamples < WINDOW_BLOCKS * reader.blockSize)
            return;

        // Drift and correction move the level slowly; any faster change is scheduling jitter.
        const double maxStep = MAX_LEVEL_RATE * windowReaderSamples * writer.sampleRate / reader.sampleRate;
        trackedLevel += std::clamp(windowMinLevel - trackedLevel, -maxStep, maxStep);

        const int zoneHalf = getZoneWidth() / 2;
        const int lowEdge = targetLevel - zoneHalf;
        const int highEdge = targetLevel + zoneHalf;
        const int loggedLevel = static_cast<int>(std::lround(trackedLevel));

        if (correctionDirection == 0)
        {
            correctionDirection = trackedLevel < lowEdge ? -1 : trackedLevel > highEdge ? 1 : 0;
            if (correctionDirection != 0)
                logState("CORRECTING", loggedLevel, targetLevel);
        }
        else if (correctionDirection * (trackedLevel - targetLevel) <= 0)
        {
            correctionDirection = 0;
            logState("SETTLED", loggedLevel, targetLevel);
        }

        // Eased in rather than stepped, so the ratio never jumps.
        const double slew = CORRECTION_AUTHORITY / CORRECTION_SLEW_WINDOWS;
        const double wanted = correctionDirection * CORRECTION_AUTHORITY;
        bufferCompensation += std::clamp(wanted - bufferCompensation, -slew, slew);

        // Matched clocks coasting inside the zone: an exact 1.0 ratio makes the resampler copy verbatim.
        const bool wasUnity = isUnity;
        isUnity = bufferCompensation == 0.0 && writer.sampleRate == reader.sampleRate;

        if (isUnity && !wasUnity)
        {
            for (auto& interpolator : interpolators)
                interpolator.snapPhase();
            logState("UNITY", loggedLevel, targetLevel);
        }

        windowMinLevel = INT_MAX;
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
        );
    }

    // Touches no reader/writer state, so it is safe from either thread and without locks.
    void logDrop([[maybe_unused]] const char* reason, [[maybe_unused]] int64_t droppedSamples) const
    {
        ATK_DBG(logPrefix() << "DROP " << reason << " samples=" << droppedSamples);
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
    // Control window in reader blocks, so detection lag scales with the buffer instead of wall time.
    static constexpr int WINDOW_BLOCKS = 8;
    // Control windows taken to reach full authority.
    static constexpr double CORRECTION_SLEW_WINDOWS = 8.0;
    // Fastest real level change: worst-case clock drift plus full correction.
    static constexpr double MAX_LEVEL_RATE = 2.0 * CORRECTION_AUTHORITY;

    juce::String tag;
    std::atomic_bool isPrepared{false};

    StreamFormat reader;
    StreamFormat writer;

    FifoBuffer2 fifo;
    std::vector<atk::Interpolator> interpolators;
    juce::AudioBuffer<float> scratch;

    bool isPriming{true};
    bool isUnity{false};
    int64_t overflowDroppedSamples{0};
    int windowMinLevel{INT_MAX};
    int windowReaderSamples{0};
    int correctionDirection{0};
    double trackedLevel{0.0};
    double bufferCompensation{0.0};

    std::mutex readLock;
    std::mutex writeLock;
};
