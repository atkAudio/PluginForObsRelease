#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

static constexpr auto FIXED_BUFFER_SIZE = 65536; // max samples
static constexpr auto TARGET_LEVEL_FACTOR = 1.5; // Target buffer level factor

// Simple FIFO buffer - no auto-growth, just basic read/write
class FifoBuffer2
{
public:
    FifoBuffer2()
        : buffer(2, 2)
        , fifo(2)
    {
    }

    void setSize(int numChannels, int numSamples)
    {
        juce::ScopedLock lock1(writeLock);
        juce::ScopedLock lock2(readLock);

        if (buffer.getNumSamples() < numSamples + 1)
            buffer.setSize(numChannels, numSamples + 1, false, false, true);

        if (fifo.getTotalSize() < numSamples + 1)
            fifo.setTotalSize(numSamples + 1);

        buffer.clear();
        fifo.reset();
    }

    int write(const float* const* src, int numChannels, int numSamples)
    {
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
    juce::AudioBuffer<float> buffer;
    juce::AbstractFifo fifo;

    juce::CriticalSection writeLock;
    juce::CriticalSection readLock;
};

// SyncBuffer: Sample rate converting buffer with fixed capacity
class SyncBuffer : private juce::AsyncUpdater
{
public:
    SyncBuffer()
        : isPrepared(false)
        , readerBufferSize(-1)
        , writerBufferSize(-1)
    {
    }

    ~SyncBuffer() override
    {
        cancelPendingUpdate();
    }

    // Force synchronous preparation (for testing)
    void forcePrepare()
    {
        juce::ScopedLock lock1(writeLock);
        juce::ScopedLock lock2(readLock);

        if (!isPrepared)
            prepareInternal();

        // For testing: enable writes immediately
        hasReadOnce.store(true, std::memory_order_release);
    }

    void clearPrepared()
    {
        juce::ScopedLock lock1(writeLock);
        juce::ScopedLock lock2(readLock);

        readerBufferSize = -1;
        writerBufferSize = -1;
        isPrepared = false;
    }

    void prepare()
    {
        juce::ScopedLock lock1(writeLock);
        juce::ScopedLock lock2(readLock);

        prepareInternal();
    }

    void prepareInternal()
    {
        // Must be called with both locks held!
        isPrepared = false;

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

        // Fixed capacity buffer
        fifoBuffer.setSize(numChannels, FIXED_BUFFER_SIZE);

        // Allocate temp buffer large enough for worst case
        tempBuffer.setSize(numChannels, FIXED_BUFFER_SIZE, false, false, true);

        // Initialize buffer level tracking
        readCallCount = 0;
        hasReadOnce.store(false, std::memory_order_release); // Reset read flag - writes will fail until first read
        historyIndex = 0;
        // Initialize with high value so actual readings will replace it
        bufferLevelHistory.assign(BUFFER_HISTORY_SIZE, FIXED_BUFFER_SIZE);

        bufferCompensation = 0.0;
        wasAtTargetLevel = false;

        isPrepared = true;
    }

    int write(const float* const* src, int numChannels, int numSamples, double sampleRate)
    {
        juce::ScopedTryLock lock(writeLock);
        if (!lock.isLocked())
            return 0;

        // Check if writer parameters changed (only grow, don't shrink)
        auto newMinBufferSize = numSamples;
        if (writerNumChannels < numChannels || writerBufferSize < newMinBufferSize || writerSampleRate != sampleRate)
        {
            writerNumChannels = numChannels;
            writerBufferSize = numSamples;
            writerSampleRate = sampleRate;

            // Trigger async preparation if both reader and writer are set
            if (readerNumChannels > 0 && readerSampleRate > 0.0 && readerBufferSize > 0)
            {
                // Mark as not prepared and trigger async re-preparation
                isPrepared = false;
                triggerAsyncUpdate();
            }

            // Return 0 since parameters changed (prepared or not)
            return 0;
        }

        // Not prepared yet - fail gracefully (let writes fail until async prepare completes)
        if (!isPrepared)
            return 0;

        // Writes fail until first read has been called - allows buffer to build up
        if (!hasReadOnce.load(std::memory_order_acquire))
            return 0;

        return fifoBuffer.write(src, numChannels, numSamples);
    }

    bool read(float* const* dest, int numChannels, int numSamples, double sampleRate, bool addToBuffer = false)
    {
        juce::ScopedTryLock lock(readLock);
        if (!lock.isLocked())
            return false;

        // Check if reader parameters changed (only grow, don't shrink)
        if (readerNumChannels < numChannels || readerSampleRate != sampleRate || readerBufferSize < numSamples)
        {
            readerNumChannels = numChannels;
            readerBufferSize = numSamples;
            readerSampleRate = sampleRate;

            // Trigger async preparation if both reader and writer are set
            if (writerNumChannels > 0 && writerSampleRate > 0.0 && writerBufferSize > 0)
            {
                // Mark as not prepared and trigger async re-preparation
                isPrepared = false;
                triggerAsyncUpdate();
            }

            // Return false since parameters changed (prepared or not)
            return false;
        }

        // Not prepared yet - fail gracefully (let reads fail until async prepare completes)
        if (!isPrepared)
            return false;

        // Mark that read has been called at least once - enables writes
        hasReadOnce.store(true, std::memory_order_release);

        auto ratio = writerSampleRate / readerSampleRate;

        int samplesInFifo = fifoBuffer.getFifo().getNumReady();
        int totalSize = fifoBuffer.getFifo().getTotalSize();

        // Store current buffer level in ring buffer
        bufferLevelHistory[historyIndex] = samplesInFifo;

        readCallCount++;

        // Move to next ring buffer position
        historyIndex = (historyIndex + 1) % BUFFER_HISTORY_SIZE;

        // Calculate buffer compensation using ring buffer
        if (readCallCount >= BUFFER_HISTORY_SIZE)
        {
            // Find minimum buffer level over the ring buffer window
            int minBufferLevel = samplesInFifo;
            for (int i = 0; i < BUFFER_HISTORY_SIZE; ++i)
                minBufferLevel = std::min(minBufferLevel, bufferLevelHistory[i]);

            // Target: reader buffer size (in writer's domain) for safety margin
            int baseTargetLevel = static_cast<int>(std::ceil(readerBufferSize * ratio));
            int targetMinLevel = static_cast<int>(std::ceil(baseTargetLevel * TARGET_LEVEL_FACTOR));
            targetMinLevel++;
            int bufferLevelError = minBufferLevel - targetMinLevel;

            // Calculate compensation: normalize error by total samples read in window
            if (bufferLevelError != 0)
            {
                juce::int64 samplesReadInWindow = static_cast<juce::int64>(readerBufferSize) * BUFFER_HISTORY_SIZE;
                bufferCompensation = static_cast<double>(bufferLevelError) / samplesReadInWindow;
            }
            else
            {
                bufferCompensation = 0.0;
            }

            // Log only when recovering/achieving target (transitioning from below to at/above target)
            bool currentlyAtTarget = (minBufferLevel >= targetMinLevel);
            if (currentlyAtTarget && !wasAtTargetLevel)
            {
                auto timestamp = juce::Time::getCurrentTime().formatted("%H:%M:%S");
                DBG("[SYNC] "
                    << timestamp
                    << " Buffer level RECOVERED - min = "
                    << minBufferLevel
                    << ", target = "
                    << targetMinLevel);
            }
            wasAtTargetLevel = currentlyAtTarget;
        }

        // Apply buffer compensation to ratio
        auto compensatedRatio = ratio * (1.0 + bufferCompensation);

        int writerSamplesNeeded = static_cast<int>(std::ceil(numSamples * compensatedRatio));

        writerSamplesNeeded++; // need one extra sample for interpolation safety

        if (!addToBuffer)
            for (int ch = 0; ch < numChannels; ++ch)
                std::memset(dest[ch], 0, sizeof(float) * numSamples);

        // Try to read what we need from FIFO - it will return what's actually available
        // Verify tempBuffer is large enough (should be pre-allocated in prepare())
        tempBuffer.setSize(writerNumChannels, writerSamplesNeeded, false, false, true);
        auto writerSamples =
            fifoBuffer.read(tempBuffer.getArrayOfWritePointers(), writerNumChannels, writerSamplesNeeded, false);

        // If we got no samples at all, fail
        if (writerSamples == 0)
            return false;

        // Adjust ratio based on what we actually got
        auto finalRatio = compensatedRatio;
        if (writerSamples < writerSamplesNeeded)
        {
            // Scale ratio to match available samples
            double availabilityFactor = static_cast<double>(writerSamples) / writerSamplesNeeded;
            finalRatio = compensatedRatio * availabilityFactor;

            auto timestamp = juce::Time::getCurrentTime().formatted("%H:%M:%S");
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
        }
        int maxSamplesConsumed = 0;

        auto channelGain = 1.0f;
        if (writerNumChannels > numChannels)
            channelGain = static_cast<float>(std::sqrt(static_cast<double>(numChannels) / writerNumChannels));

        // Process all samples at once per channel
        for (int destCh = 0; destCh < numChannels; ++destCh)
        {
            int srcCh = destCh % writerNumChannels;

            int samplesConsumed =
                interpolators[srcCh]
                    .process(finalRatio, tempBuffer.getReadPointer(srcCh), dest[destCh], numSamples, writerSamples, 0);

            // Apply channel gain
            if (channelGain != 1.0f)
                for (int j = 0; j < numSamples; ++j)
                    dest[destCh][j] *= channelGain;

            maxSamplesConsumed = std::max(maxSamplesConsumed, samplesConsumed);
        }

        // Handle extra writer channels (fold down into reader channels)
        if (writerNumChannels > numChannels)
        {
            for (int i = numChannels; i < writerNumChannels; ++i)
            {
                auto destCh = i % numChannels;

                // Process into temp location then mix
                float tempOutput[FIXED_BUFFER_SIZE];
                int samplesConsumed =
                    interpolators[i]
                        .process(finalRatio, tempBuffer.getReadPointer(i), tempOutput, numSamples, writerSamples, 0);

                // Mix with gain into destination
                for (int j = 0; j < numSamples; ++j)
                    dest[destCh][j] += tempOutput[j] * channelGain;

                maxSamplesConsumed = std::max(maxSamplesConsumed, samplesConsumed);
            }
        }

        // Advance by what we actually read from FIFO (not what interpolator consumed)
        // The interpolator can consume more than available during underflow due to its internal state
        int samplesToAdvance = std::min(maxSamplesConsumed, writerSamples);

        if (maxSamplesConsumed > writerSamples)
        {
            DBG("[SYNC] INFO - Interpolator consumed "
                << maxSamplesConsumed
                << " but only "
                << writerSamples
                << " available - advancing by "
                << samplesToAdvance);
        }

        fifoBuffer.advanceRead(samplesToAdvance);

        return true;
    }

private:
    bool isPrepared{false};
    int numChannels{0};

    FifoBuffer2 fifoBuffer;
    std::vector<juce::LagrangeInterpolator> interpolators;

    juce::AudioBuffer<float> tempBuffer;

    int readerBufferSize{0};
    int writerBufferSize{0};
    int readerNumChannels{0};
    int writerNumChannels{0};

    double readerSampleRate{0.0};
    double writerSampleRate{0.0};

    // Buffer level tracking with ring buffer (tracks last N read calls)
    static constexpr int BUFFER_HISTORY_SIZE = 1024; // Number of read() calls to track

    // Ring buffer to track buffer levels
    std::vector<int> bufferLevelHistory;  // Ring buffer of last BUFFER_HISTORY_SIZE buffer levels
    int historyIndex{0};                  // Current position in ring buffer
    int readCallCount{0};                 // Total number of read() calls
    std::atomic<bool> hasReadOnce{false}; // Flag to enable writes after first read

    double bufferCompensation{0.0}; // Current compensation value
    bool wasAtTargetLevel{false};   // Track if we were at target level (for recovery detection)

    juce::CriticalSection readLock;
    juce::CriticalSection writeLock;

    // AsyncUpdater callback for realtime-safe initialization
    void handleAsyncUpdate() override
    {
        // Called on message thread - safe to allocate memory
        // Audio threads will fail their reads/writes until prepare() completes
        prepare();
    }
};
