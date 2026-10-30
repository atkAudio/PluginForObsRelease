#include "FifoBuffer2.h"
#include <limits>

namespace
{
bool PositiveFinite(double value)
{
    return value > 0.0 && std::isfinite(value);
}

bool NonnegativeFinite(double value)
{
    return value >= 0.0 && std::isfinite(value);
}

bool StorageFits(int channels, int capacity, int maxWriteFrames)
{
    return channels > 0
        && channels < std::numeric_limits<int>::max()
        && capacity > 0
        && capacity < std::numeric_limits<int>::max()
        && maxWriteFrames > 0
        && maxWriteFrames <= capacity;
}

double TargetFrames(int maxReadFrames, double nominalRatio, int framesRead = 0)
{
    return std::ceil((2.0 * maxReadFrames - framesRead) * nominalRatio);
}

// Phase can carry at most one preceding ratio step into the next read.
double ScratchFrames(int maxReadFrames, double maximumRatio)
{
    return std::ceil((static_cast<double>(maxReadFrames) + 1.0) * maximumRatio) + 1.0;
}
} // namespace

struct SyncBuffer::StatsLogger : juce::Timer
{
    struct Snapshot
    {
        Diagnostics diagnostics;
        int target{0};
        double elapsedSeconds{0.0};
    };

    enum
    {
        newSnapshot = 4,
        indexMask = 3
    };

    explicit StatsLogger(const juce::String& debugTag)
        : tag(debugTag)
    {
        startTimer(5000);
    }

    void setTag(const juce::String& debugTag)
    {
        std::lock_guard lock(tagLock);
        tag = debugTag;
    }

    void publish(Diagnostics diagnostics, int target, double elapsedSeconds)
    {
        snapshots[writeIndex] = {diagnostics, target, elapsedSeconds};
        // The reader, producer, and handoff each own a different slot.
        writeIndex = pending.exchange(writeIndex | newSnapshot, std::memory_order_acq_rel) & indexMask;
    }

    void timerCallback()
    {
        if (pending.load(std::memory_order_acquire) & newSnapshot)
            readIndex = pending.exchange(readIndex, std::memory_order_acq_rel) & indexMask;
        Snapshot snapshot = snapshots[readIndex];
        Diagnostics& state = snapshot.diagnostics;
        if (state.callbackFrames == 0)
            return;
        state.overflows = overflows.load(std::memory_order_relaxed);
        juce::String debugTag;
        {
            std::lock_guard lock(tagLock);
            debugTag = tag;
        }
        ATK_DBG(
            "[SYNC] "
            << debugTag
            << " STATUS st="
            << (state.state == running      ? "RUNNING"
                : state.state == recovering ? "RECOVERING"
                                            : "PREFILL")
            << " n="
            << state.callbackFrames
            << " lvl="
            << state.availableFrames
            << " tgt="
            << snapshot.target
            << " need="
            << state.actualNeed
            << " err="
            << state.levelErrorFrames
            << " low="
            << state.latestTrough
            << " corr="
            << state.correction
            << " urun="
            << state.underruns
            << " urunph="
            << juce::String(snapshot.elapsedSeconds > 0.0 ? state.underruns * 3600.0 / snapshot.elapsedSeconds : 0.0, 2)
            << " ovf="
            << state.overflows
            << " rec="
            << state.recoveries
            << " rst="
            << state.staleReaderResets
        );
    }

    Snapshot snapshots[3];
    std::atomic<int> pending{1};
    int writeIndex{0};
    int readIndex{2};
    std::atomic<uint64_t> overflows{0};
    juce::String tag;
    std::mutex tagLock;
};

SyncBuffer::SyncBuffer(const juce::String& debugTag)
    : SyncBuffer(StorageLimits{}, debugTag)
{
}

SyncBuffer::SyncBuffer(StorageLimits limits, const juce::String& debugTag)
    : storageLimits(limits)
{
    parameters.fifoCapacity = limits.fifoCapacity;
    parameters.maxWriteFrames = limits.maxWriteFrames;
    if (StorageFits(limits.channels, limits.fifoCapacity, limits.maxWriteFrames))
        allocateStorage(limits.channels, limits.fifoCapacity, limits.fifoCapacity);
#if defined(ATK_DEBUG) && !defined(ATK_CI_BUILD)
    statsLogger = std::make_shared<StatsLogger>(debugTag);
#else
    (void)debugTag;
#endif
}

SyncBuffer::~SyncBuffer()
{
    if (statsLogger)
    {
        statsLogger->stopTimer();
        juce::MessageManager* manager = juce::MessageManager::getInstanceWithoutCreating();
        if (manager != nullptr && !manager->isThisTheMessageThread())
        {
            // Keep the logger alive until any message-thread callback has finished.
            juce::MessageManager::callAsync([logger = std::move(statsLogger)] { logger->stopTimer(); });
        }
    }
}

void SyncBuffer::setTag(const juce::String& debugTag)
{
    if (statsLogger)
        statsLogger->setTag(debugTag);
}

void SyncBuffer::allocateStorage(int channels, int capacity, int scratchFrames)
{
    fifo.setSize(channels, capacity);
    interpolators.resize(channels);
    scratch.setSize(channels, scratchFrames);
}

bool SyncBuffer::prepare(Parameters next)
{
    return prepareStream(next, true);
}

bool SyncBuffer::prepareStream(Parameters next, bool useFixedFormat)
{
    if (!PositiveFinite(next.inputRate)
        || !PositiveFinite(next.outputRate)
        || !StorageFits(next.channels, next.fifoCapacity, next.maxWriteFrames)
        || next.maxReadFrames <= 0
        || !PositiveFinite(next.correctionTimeSeconds)
        || !NonnegativeFinite(next.correctionLimit)
        || next.correctionLimit >= 1.0
        || !NonnegativeFinite(next.readerStallPeriods)
        || next.fadeFrames < 0)
        return false;

    double maximumRatio = next.inputRate / next.outputRate * (1.0 + next.correctionLimit);
    double scratchFrames = ScratchFrames(next.maxReadFrames, maximumRatio);
    double targetFrames = TargetFrames(next.maxReadFrames, next.inputRate / next.outputRate);
    if (!PositiveFinite(maximumRatio)
        || !PositiveFinite(next.inputRate / next.outputRate * (1.0 - next.correctionLimit))
        || std::max(scratchFrames, targetFrames) > next.fifoCapacity)
        return false;

    std::scoped_lock lock(writeLock, readLock);
    isPrepared.store(false, std::memory_order_release);
    parameters = next;
    fixedFormat = useFixedFormat;
    allocateStorage(
        fixedFormat ? next.channels : std::max(storageLimits.channels, next.channels),
        next.fifoCapacity,
        fixedFormat ? static_cast<int>(scratchFrames) : next.fifoCapacity
    );
    writer = {next.channels, next.maxWriteFrames, next.inputRate};
    reader = {next.channels, next.maxReadFrames, next.outputRate};
    restartStream();
    return true;
}

void SyncBuffer::prepare(int numChannels, int bufferSize, double sampleRate)
{
    Parameters next;
    next.channels = numChannels;
    next.maxReadFrames = bufferSize;
    next.maxWriteFrames = std::max(storageLimits.maxWriteFrames, bufferSize);
    next.fifoCapacity = storageLimits.fifoCapacity;
    next.inputRate = next.outputRate = sampleRate;
    prepareStream(next, false);
}

void SyncBuffer::clearPrepared()
{
    std::scoped_lock lock(writeLock, readLock);
    writer = {};
    reader = {};
    restartStream();
}

void SyncBuffer::reset()
{
    std::scoped_lock lock(writeLock, readLock);
    restartStream();
}

bool SyncBuffer::getIsPrepared() const
{
    return isPrepared.load(std::memory_order_acquire);
}

int SyncBuffer::getNumReady() const
{
    return fifo.getNumReady();
}

int SyncBuffer::getTargetLevel() const
{
    return targetLevel.load(std::memory_order_relaxed);
}

double SyncBuffer::getBufferCompensation() const
{
    return compensation.load(std::memory_order_relaxed);
}

SyncBuffer::Diagnostics SyncBuffer::getDiagnostics()
{
    std::lock_guard lock(readLock);
    Diagnostics snapshot = diagnostics;
    snapshot.overflows = overflows.load(std::memory_order_relaxed);
    return snapshot;
}

bool SyncBuffer::formatsFit(StreamFormat input, StreamFormat output)
{
    if (input.channels <= 0 || output.channels <= 0 || !PositiveFinite(input.rate) || !PositiveFinite(output.rate))
        return false;
    double maximumRatio = input.rate / output.rate * (1.0 + parameters.correctionLimit);
    double scratchFrames = ScratchFrames(output.maxFrames, maximumRatio);
    double targetFrames = TargetFrames(output.maxFrames, input.rate / output.rate);
    return PositiveFinite(maximumRatio)
        && PositiveFinite(input.rate / output.rate * (1.0 - parameters.correctionLimit))
        && scratchFrames <= scratch.getNumSamples()
        && std::max(scratchFrames, targetFrames) <= parameters.fifoCapacity;
}

void SyncBuffer::restartStream()
{
    fifo.reset();
    for (auto& interpolator : interpolators)
        interpolator.reset();
    diagnostics = {};
    bool ready = formatsFit(writer, reader);
    clearMeasurement();
    overflows.store(0, std::memory_order_relaxed);
    if (statsLogger)
        statsLogger->overflows.store(0, std::memory_order_relaxed);
    compensation.store(0.0, std::memory_order_relaxed);
    fadePosition = 0;
    hasReadTime = false;
    lastReadTime = 0.0;
    targetLevel.store(
        ready ? static_cast<int>(TargetFrames(reader.maxFrames, writer.rate / reader.rate)) : 0,
        std::memory_order_relaxed
    );
    isPrepared.store(ready, std::memory_order_release);
    publishStats();
}

void SyncBuffer::clearMeasurement()
{
    previousLevel = 0;
    hasPreviousLevel = false;
    wasFalling = false;
    diagnostics.latestTrough = 0.0;
    diagnostics.hasTrough = false;
}

void SyncBuffer::updateMeasurement()
{
    int level = diagnostics.availableFrames;
    if (hasPreviousLevel && (level == previousLevel || (level > previousLevel && wasFalling)))
    {
        diagnostics.latestTrough = previousLevel;
        diagnostics.hasTrough = true;
    }
    else if (diagnostics.hasTrough && level < diagnostics.latestTrough)
    {
        // Follow a new low immediately, rather than waiting for replenishment to confirm it.
        diagnostics.latestTrough = level;
    }
    wasFalling = hasPreviousLevel && level < previousLevel;
    previousLevel = level;
    hasPreviousLevel = true;
}

int SyncBuffer::write(const float* const* src, int channels, int frames, double rate)
{
    if (channels <= 0 || frames <= 0 || !PositiveFinite(rate))
        return 0;
    std::unique_lock lock(writeLock, std::try_to_lock);
    if (!lock.owns_lock() || channels > scratch.getNumChannels() || frames > parameters.maxWriteFrames)
        return 0;

    if (channels != writer.channels || rate != writer.rate)
    {
        if (fixedFormat)
            return 0;
        std::unique_lock readerLock(readLock, std::try_to_lock);
        if (!readerLock.owns_lock())
            return 0;
        StreamFormat nextWriter{channels, parameters.maxWriteFrames, rate};
        if (reader.rate > 0.0 && !formatsFit(nextWriter, reader))
            return 0;
        writer = nextWriter;
        restartStream();
    }
    if (!getIsPrepared())
        return 0;
    if (!fifo.tryWrite(src, channels, frames))
    {
        uint64_t count = overflows.fetch_add(1, std::memory_order_relaxed) + 1;
        if (statsLogger)
            statsLogger->overflows.store(count, std::memory_order_relaxed);
        return 0;
    }
    return frames;
}

int SyncBuffer::write(const float* const* src, int channels, int frames, double rate, double)
{
    return write(src, channels, frames, rate);
}

bool SyncBuffer::read(float* const* dest, int channels, int frames, double rate)
{
    return read(dest, channels, frames, rate, juce::Time::getMillisecondCounterHiRes() * 0.001);
}

bool SyncBuffer::read(float* const* dest, int channels, int frames, double rate, double timeSeconds)
{
    if (channels <= 0 || frames <= 0)
        return false;
    for (int ch = 0; ch < channels; ++ch)
        std::fill_n(dest[ch], frames, 0.0f);
    if (!PositiveFinite(rate))
        return false;

    std::unique_lock lock(readLock, std::try_to_lock);
    if (!lock.owns_lock() || channels > scratch.getNumChannels())
        return false;
    if (channels != reader.channels || rate != reader.rate || frames > reader.maxFrames)
    {
        if (fixedFormat)
            return false;
        std::unique_lock writerLock(writeLock, std::try_to_lock);
        if (!writerLock.owns_lock())
            return false;
        StreamFormat nextReader{channels, std::max(frames, reader.maxFrames), rate};
        if (writer.rate > 0.0 && !formatsFit(writer, nextReader))
            return false;
        reader = nextReader;
        restartStream();
    }
    if (!getIsPrepared())
        return false;

    bool stalled = hasReadTime
                && parameters.readerStallPeriods > 0.0
                && timeSeconds - lastReadTime > parameters.readerStallPeriods * reader.maxFrames / reader.rate;
    if (!hasReadTime)
        firstReadTime = timeSeconds;
    lastReadTime = timeSeconds;
    hasReadTime = true;
    if (stalled)
    {
        // Only the consumer advances its position; input published after this snapshot stays queued.
        fifo.advanceRead(fifo.getNumReady());
        for (auto& interpolator : interpolators)
            interpolator.reset();
        clearMeasurement();
        diagnostics.state = prefill;
        diagnostics.prefillFrames = 0;
        ++diagnostics.staleReaderResets;
        diagnostics.callbackFrames = frames;
        diagnostics.availableFrames = 0;
        diagnostics.actualNeed = 0;
        diagnostics.levelErrorFrames = 0.0;
        fadePosition = 0;
        publishStats();
        return false;
    }

    bool rendered = render(dest, channels, frames);
    publishStats();
    return rendered;
}

void SyncBuffer::publishStats()
{
    if (statsLogger)
        statsLogger->publish(diagnostics, getTargetLevel(), hasReadTime ? lastReadTime - firstReadTime : 0.0);
}

bool SyncBuffer::render(float* const* dest, int channels, int frames)
{
    double nominalRatio = writer.rate / reader.rate;
    int targetFrames = getTargetLevel();
    double correction =
        diagnostics.hasTrough
            ? std::clamp(
                  (diagnostics.latestTrough - targetFrames) / writer.rate / parameters.correctionTimeSeconds,
                  -parameters.correctionLimit,
                  parameters.correctionLimit
              )
            : diagnostics.correction;
    double ratio = nominalRatio * (1.0 + correction);
    diagnostics.callbackFrames = frames;
    diagnostics.availableFrames = fifo.getNumReady();
    diagnostics.actualNeed = interpolators.front().getRequiredInputSamples(ratio, frames);
    diagnostics.levelErrorFrames = diagnostics.availableFrames - targetFrames;

    if (diagnostics.state != running)
    {
        if (diagnostics.availableFrames > 0 && diagnostics.prefillFrames < reader.maxFrames)
        {
            diagnostics.prefillFrames += std::min(frames, reader.maxFrames - diagnostics.prefillFrames);
            return false;
        }
        if (diagnostics.availableFrames < targetFrames)
            return false;
        if (diagnostics.state == recovering)
            ++diagnostics.recoveries;
        diagnostics.state = running;
        fadePosition = 0;
        clearMeasurement();
    }

    if (diagnostics.availableFrames < diagnostics.actualNeed)
    {
        diagnostics.state = recovering;
        diagnostics.prefillFrames = 0;
        ++diagnostics.underruns;
        clearMeasurement();
        return false;
    }

    int needed = diagnostics.actualNeed;
    fifo.peek(scratch.getArrayOfWritePointers(), writer.channels, needed);
    float foldGain = writer.channels > channels ? std::sqrt(static_cast<float>(channels) / writer.channels) : 1.0f;
    int consumed = 0;
    for (int ch = 0; ch < writer.channels; ++ch)
        consumed = interpolators[ch].processAdding(
            ratio,
            scratch.getReadPointer(ch),
            dest[ch % channels],
            frames,
            needed,
            ch < channels ? 1.0f : foldGain
        );
    fifo.advanceRead(consumed);
    for (int sample = 0; sample < frames && fadePosition < parameters.fadeFrames; ++sample, ++fadePosition)
        for (int ch = 0; ch < channels; ++ch)
            dest[ch][sample] *= static_cast<float>(fadePosition + 1) / parameters.fadeFrames;

    updateMeasurement();
    double remainingTarget = TargetFrames(reader.maxFrames, nominalRatio, frames);
    if (diagnostics.hasTrough && diagnostics.availableFrames - consumed == remainingTarget)
    {
        // Use this read's snapshot so concurrent writes cannot hide reaching the target.
        clearMeasurement();
        diagnostics.latestTrough = targetFrames;
        diagnostics.hasTrough = true;
        correction = 0.0;
    }
    diagnostics.correction = correction;
    compensation.store(correction, std::memory_order_relaxed);

    return true;
}
