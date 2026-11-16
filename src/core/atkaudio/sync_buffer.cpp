#include "FifoBuffer2.h"
#include <limits>

namespace
{
constexpr double TIMELINE_BANDWIDTH_HZ = 0.1;
// About 1.7 cents: covers real clock mismatch with headroom while staying inaudible.
constexpr double MAX_CORRECTION = 1000.0e-6;
constexpr double DROP_MINIMUM_SECONDS = 0.02;
// An early read is a one-read event; a mean over a few reads keeps the controller's own ramps out of it.
constexpr double DIP_MEAN_READS = 8.0;
constexpr double PEAK_RENEWAL_FRACTION = 0.9;

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

// Phase can carry at most one preceding ratio step into the next read.
double ScratchFrames(int maxReadFrames, double nominalRatio)
{
    return std::ceil((maxReadFrames + 1.0) * nominalRatio * (1.0 + MAX_CORRECTION)) + 1.0;
}

// The FIFO must hold one maximum read alongside one maximum write.
double RequiredCapacity(int maxReadFrames, int maxWriteFrames, double nominalRatio)
{
    return std::max(
        ScratchFrames(maxReadFrames, nominalRatio),
        std::ceil(maxReadFrames * nominalRatio) + maxWriteFrames
    );
}

void FadeIn(float* const* dest, int channels, int frames, int fadeFrames, int* position)
{
    for (int sample = 0; sample < frames && *position < fadeFrames; ++sample, ++*position)
        for (int ch = 0; ch < channels; ++ch)
            dest[ch][sample] *= static_cast<float>(*position + 1) / fadeFrames;
}

void FadeOut(float* const* dest, int channels, int frames, int fadeFrames)
{
    int length = std::min(frames, fadeFrames);
    for (int sample = frames - length; sample < frames; ++sample)
        for (int ch = 0; ch < channels; ++ch)
            dest[ch][sample] *= static_cast<float>(frames - sample) / (length + 1);
}
} // namespace

double atk::TrackTimeline(
    StreamTimeline* timeline,
    int64_t frames,
    double timeSeconds,
    double rate,
    double intervalSeconds,
    double resyncSeconds
)
{
    if (!timeline->isValid)
    {
        timeline->originSeconds = timeSeconds - frames / rate;
        timeline->isValid = true;
        return 0.0;
    }
    double error = timeSeconds - (timeline->originSeconds + frames / rate);
    if (error < 0.0 || error > resyncSeconds)
        timeline->originSeconds += error;
    else
        timeline->originSeconds +=
            std::min(1.0, juce::MathConstants<double>::twoPi * TIMELINE_BANDWIDTH_HZ * intervalSeconds) * error;
    return error;
}

void atk::HoldPeak(PeakHold* peak, double value, double timeSeconds, double holdSeconds, double releasePerSecond)
{
    if (value >= peak->value)
    {
        peak->value = value;
        peak->raisedValue = value;
        peak->peakSeconds = timeSeconds;
    }
    else if (value >= PEAK_RENEWAL_FRACTION * peak->raisedValue)
    {
        // A recurrence near the peak (jitter makes it rarely exact) proves the reserve is still needed.
        // Comparing with the raise, not the released value, lets ordinary values finish the release.
        peak->peakSeconds = timeSeconds;
    }
    else if (timeSeconds - peak->peakSeconds > holdSeconds)
    {
        peak->value = std::max(value, peak->value - releasePerSecond * (timeSeconds - peak->lastSeconds));
    }
    peak->lastSeconds = timeSeconds;
}

struct SyncBuffer::StatsLogger : juce::Timer
{
    struct Snapshot
    {
        Diagnostics diagnostics;
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

    void publish(Diagnostics diagnostics, double elapsedSeconds)
    {
        snapshots[writeIndex] = {diagnostics, elapsedSeconds};
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
            << " avail="
            << state.availableFrames
            << " need="
            << state.actualNeed
            << " lvl="
            << juce::String(state.levelFrames, 1)
            << " tgt="
            << juce::String(state.targetFrames, 1)
            << " wres="
            << juce::String(state.writerReserveFrames, 1)
            << " rres="
            << juce::String(state.readerReserveFrames, 1)
            << " err="
            << juce::String(state.errorFrames, 1)
            << " filtered="
            << juce::String(state.filteredErrorFrames, 1)
            << " corr="
            << state.correction
            << " drift="
            << state.driftCorrection
            << " urun="
            << state.underruns
            << " urunph="
            << juce::String(snapshot.elapsedSeconds > 0.0 ? state.underruns * 3600.0 / snapshot.elapsedSeconds : 0.0, 2)
            << " ovf="
            << state.overflows
            << " rec="
            << state.recoveries
            << " drop="
            << state.drops
            << " stall="
            << state.writerStalls
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
    : storage(limits)
{
    if (StorageFits(storage.channels, storage.fifoCapacity, storage.maxWriteFrames))
        allocateStorage();
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

void SyncBuffer::allocateStorage()
{
    fifo.setSize(storage.channels, storage.fifoCapacity);
    interpolators.resize(storage.channels);
    scratch.setSize(storage.channels, storage.fifoCapacity);
}

bool SyncBuffer::prepare(int numChannels, int bufferSize, double sampleRate)
{
    StorageLimits next = storage;
    next.channels = std::max(storage.channels, numChannels);
    next.maxWriteFrames = std::max(storage.maxWriteFrames, bufferSize);
    if (numChannels <= 0
        || bufferSize <= 0
        || !PositiveFinite(sampleRate)
        || !StorageFits(next.channels, next.fifoCapacity, next.maxWriteFrames)
        || RequiredCapacity(bufferSize, next.maxWriteFrames, 1.0) > next.fifoCapacity)
        return false;

    std::scoped_lock lock(writeLock, readLock);
    isPrepared.store(false, std::memory_order_release);
    storage = next;
    allocateStorage();
    writer = {numChannels, storage.maxWriteFrames, sampleRate};
    reader = {numChannels, bufferSize, sampleRate};
    restartStream();
    return getIsPrepared();
}

bool SyncBuffer::setTuning(Tuning next)
{
    if (!PositiveFinite(next.correctionTimeSeconds)
        || !PositiveFinite(next.errorFilterSeconds)
        || !PositiveFinite(next.maxLatenessSeconds)
        || !NonnegativeFinite(next.reserveHoldSeconds)
        || !PositiveFinite(next.reserveReleaseRate)
        || !NonnegativeFinite(next.safetySeconds)
        || !NonnegativeFinite(next.readerStallPeriods)
        || next.fadeFrames < 0)
        return false;
    std::scoped_lock lock(writeLock, readLock);
    tuning = next;
    return true;
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
    snapshot.writerStalls = writerStalls.load(std::memory_order_relaxed);
    return snapshot;
}

bool SyncBuffer::formatsFit(StreamFormat input, StreamFormat output)
{
    if (input.channels <= 0 || output.channels <= 0 || !PositiveFinite(input.rate) || !PositiveFinite(output.rate))
        return false;
    double nominalRatio = input.rate / output.rate;
    return PositiveFinite(nominalRatio)
        && ScratchFrames(output.maxFrames, nominalRatio) <= scratch.getNumSamples()
        && RequiredCapacity(output.maxFrames, input.maxFrames, nominalRatio) <= storage.fifoCapacity;
}

// Both locks are held: every writer- and reader-owned value starts over.
void SyncBuffer::restartStream()
{
    fifo.reset();
    for (auto& interpolator : interpolators)
        interpolator.reset();
    diagnostics = {};
    writerTimeline = {};
    writerLateness = {};
    writtenFrames = 0;
    writerOrigin.store(0.0, std::memory_order_relaxed);
    writerReserve.store(0.0, std::memory_order_relaxed);
    hasWriterTimeline.store(false, std::memory_order_release);
    hasWriterReserve.store(false, std::memory_order_release);
    writerStalls.store(0, std::memory_order_relaxed);
    overflows.store(0, std::memory_order_relaxed);
    if (statsLogger)
        statsLogger->overflows.store(0, std::memory_order_relaxed);
    readerDemand = {};
    meanLevel = 0.0;
    previousTarget = 0.0;
    lastTargetFallSeconds = 0.0;
    consumedFrames = 0;
    compensation.store(0.0, std::memory_order_relaxed);
    targetLevel.store(0, std::memory_order_relaxed);
    fadePosition = 0;
    hasReadTime = false;
    lastReadTime = 0.0;
    isPrepared.store(formatsFit(writer, reader), std::memory_order_release);
    publishStats();
}

// The reader starts over on fresh input; the writer's timeline and the learned drift are kept.
void SyncBuffer::restartReader()
{
    // Only the consumer advances its position; input published after this snapshot stays queued.
    int stale = fifo.getNumReady();
    fifo.advanceRead(stale);
    consumedFrames += stale;
    for (auto& interpolator : interpolators)
        interpolator.reset();
    readerDemand = {};
    previousTarget = 0.0;
    lastTargetFallSeconds = 0.0;
    diagnostics.state = prefill;
    diagnostics.filteredErrorFrames = 0.0;
    diagnostics.correction = diagnostics.driftCorrection;
    compensation.store(diagnostics.correction, std::memory_order_relaxed);
    fadePosition = 0;
}

void SyncBuffer::trackWrite(int frames, double timeSeconds)
{
    writtenFrames += frames;
    bool hadTimeline = writerTimeline.isValid;
    double error = atk::TrackTimeline(
        &writerTimeline,
        writtenFrames,
        timeSeconds,
        writer.rate,
        frames / writer.rate,
        tuning.maxLatenessSeconds
    );
    // The first delivery has no predecessor, so its size says nothing about lateness (it may be a backlog).
    // A delivery this late is a stall: the timeline resyncs and the reserve keeps covering normal jitter.
    bool stalled = error > tuning.maxLatenessSeconds;
    if (stalled)
        writerStalls.fetch_add(1, std::memory_order_relaxed);
    if (hadTimeline && !stalled)
    {
        atk::HoldPeak(
            &writerLateness,
            frames + error * writer.rate,
            timeSeconds,
            tuning.reserveHoldSeconds,
            tuning.reserveReleaseRate * writer.rate
        );
        writerReserve.store(writerLateness.value, std::memory_order_relaxed);
        hasWriterReserve.store(true, std::memory_order_release);
    }
    writerOrigin.store(writerTimeline.originSeconds, std::memory_order_relaxed);
    hasWriterTimeline.store(true, std::memory_order_release);
}

// An early read finds the level below its mean by the input the writer had yet to produce. Measuring the
// dip, rather than the reader's own timing, leaves out reads that move together with the deliveries.
void SyncBuffer::trackRead(int need, double level, double timeSeconds)
{
    double dip = 0.0;
    if (diagnostics.state == running)
    {
        dip = std::max(0.0, meanLevel - level);
        meanLevel += (level - meanLevel) / DIP_MEAN_READS;
    }
    atk::HoldPeak(
        &readerDemand,
        need + dip,
        timeSeconds,
        tuning.reserveHoldSeconds,
        tuning.reserveReleaseRate * writer.rate
    );
}

// What the writer has produced by now according to its timeline but not yet delivered.
double SyncBuffer::getUndelivered(int available, double timeSeconds)
{
    if (!hasWriterTimeline.load(std::memory_order_acquire))
        return 0.0;
    double produced = (timeSeconds - writerOrigin.load(std::memory_order_relaxed)) * writer.rate;
    return produced - (consumedFrames + available);
}

void SyncBuffer::updateCorrection(double errorFrames, double seconds, bool learnDrift)
{
    diagnostics.filteredErrorFrames +=
        (errorFrames - diagnostics.filteredErrorFrames) * std::min(1.0, seconds / tuning.errorFilterSeconds);
    double errorSeconds = diagnostics.filteredErrorFrames / writer.rate;
    double response = tuning.correctionTimeSeconds;
    double proportional = errorSeconds / response;
    double drift = diagnostics.driftCorrection + errorSeconds * seconds / (4.0 * response * response);
    // The drift estimate holds while the output is clamped, so it cannot wind up, and while a reserve
    // releases, so the slow fall of the target is not learned as clock drift.
    if (learnDrift && std::abs(drift + proportional) < MAX_CORRECTION)
        diagnostics.driftCorrection = drift;
    diagnostics.correction = std::clamp(diagnostics.driftCorrection + proportional, -MAX_CORRECTION, MAX_CORRECTION);
}

int SyncBuffer::write(const float* const* src, int channels, int frames, double rate)
{
    return write(src, channels, frames, rate, juce::Time::getMillisecondCounterHiRes() * 0.001);
}

int SyncBuffer::write(const float* const* src, int channels, int frames, double rate, double timeSeconds)
{
    if (channels <= 0 || frames <= 0 || !PositiveFinite(rate))
        return 0;
    std::unique_lock lock(writeLock, std::try_to_lock);
    if (!lock.owns_lock() || channels > scratch.getNumChannels() || frames > storage.maxWriteFrames)
        return 0;

    if (channels != writer.channels || rate != writer.rate)
    {
        std::unique_lock readerLock(readLock, std::try_to_lock);
        if (!readerLock.owns_lock())
            return 0;
        StreamFormat nextWriter{channels, storage.maxWriteFrames, rate};
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
    trackWrite(frames, timeSeconds);
    return frames;
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
        std::unique_lock writerLock(writeLock, std::try_to_lock);
        if (!writerLock.owns_lock())
            return false;
        StreamFormat nextReader{channels, std::max(frames, reader.maxFrames), rate};
        if (writer.rate > 0.0 && !formatsFit(writer, nextReader))
            return false;
        bool formatChanged = channels != reader.channels || rate != reader.rate;
        reader = nextReader;
        // A larger block only widens the storage requirement; the stream itself continues.
        if (formatChanged)
            restartStream();
    }
    if (!getIsPrepared())
        return false;

    bool firstRead = !hasReadTime;
    bool stalled = hasReadTime
                && tuning.readerStallPeriods > 0.0
                && timeSeconds - lastReadTime > tuning.readerStallPeriods * reader.maxFrames / reader.rate;
    if (firstRead)
        firstReadTime = timeSeconds;
    lastReadTime = timeSeconds;
    hasReadTime = true;
    if (firstRead || stalled)
    {
        restartReader();
        if (stalled)
            ++diagnostics.staleReaderResets;
        diagnostics.callbackFrames = frames;
        diagnostics.availableFrames = 0;
        diagnostics.actualNeed = 0;
        diagnostics.errorFrames = 0.0;
        publishStats();
        return false;
    }

    bool rendered = render(dest, channels, frames, timeSeconds);
    publishStats();
    return rendered;
}

void SyncBuffer::publishStats()
{
    if (!statsLogger)
        return;
    Diagnostics snapshot = diagnostics;
    snapshot.writerStalls = writerStalls.load(std::memory_order_relaxed);
    statsLogger->publish(snapshot, hasReadTime ? lastReadTime - firstReadTime : 0.0);
}

int SyncBuffer::renderFrames(float* const* dest, int channels, int frames, double ratio, int inputFrames)
{
    fifo.peek(scratch.getArrayOfWritePointers(), writer.channels, inputFrames);
    float foldGain = writer.channels > channels ? std::sqrt(static_cast<float>(channels) / writer.channels) : 1.0f;
    int consumed = 0;
    for (int ch = 0; ch < writer.channels; ++ch)
        consumed = interpolators[ch].processAdding(
            ratio,
            scratch.getReadPointer(ch),
            dest[ch % channels],
            frames,
            inputFrames,
            ch < channels ? 1.0f : foldGain
        );
    return consumed;
}

bool SyncBuffer::render(float* const* dest, int channels, int frames, double timeSeconds)
{
    double ratio = writer.rate / reader.rate * (1.0 + diagnostics.correction);
    int need = interpolators.front().getRequiredInputSamples(ratio, frames);
    int available = fifo.getNumReady();
    bool hasReserve = hasWriterReserve.load(std::memory_order_acquire);
    double writerReserveFrames = hasReserve ? writerReserve.load(std::memory_order_relaxed) : 0.0;
    // Never negative beyond rounding: the timeline follows the earliest deliveries.
    double undelivered = std::max(0.0, getUndelivered(available, timeSeconds));
    // A stalled writer must not look like surplus, so the controlled level counts at most its reserve.
    double level = available + std::min(undelivered, writerReserveFrames);
    // A writer later than its reserve raises its own reserve; for the reader's dips it lifts the level.
    double dipLevel = available + undelivered;
    trackRead(need, dipLevel, timeSeconds);
    double target = readerDemand.value + writerReserveFrames + tuning.safetySeconds * writer.rate;
    // A release lowers the target in steps, one per delivery; learning resumes a filter period after the last.
    if (target < previousTarget)
        lastTargetFallSeconds = timeSeconds;
    previousTarget = target;
    bool learnDrift = timeSeconds - lastTargetFallSeconds > tuning.errorFilterSeconds;
    diagnostics.callbackFrames = frames;
    diagnostics.availableFrames = available;
    diagnostics.actualNeed = need;
    diagnostics.levelFrames = level;
    diagnostics.targetFrames = target;
    diagnostics.writerReserveFrames = writerReserveFrames;
    diagnostics.readerReserveFrames = readerDemand.value;
    diagnostics.errorFrames = level - target;
    targetLevel.store(static_cast<int>(std::ceil(target)), std::memory_order_relaxed);

    if (diagnostics.state != running)
    {
        // Startup and recovery share one condition: the reserve covers both sides' timing again.
        // A writer reserve exists from the second delivery on, once the writer's cadence is known.
        if (!hasReserve || available < need || level < target)
            return false;
        if (diagnostics.state == recovering)
            ++diagnostics.recoveries;
        else
        {
            // Nothing has played yet, so a delivery that overshoots the target is skipped, not drained.
            int skipped = static_cast<int>(std::min(level - target, static_cast<double>(available - need)));
            fifo.advanceRead(skipped);
            consumedFrames += skipped;
            available -= skipped;
            level -= skipped;
            dipLevel -= skipped;
            diagnostics.errorFrames = level - target;
        }
        diagnostics.state = running;
        meanLevel = dipLevel;
        fadePosition = 0;
    }

    if (available < need)
    {
        // Play what is buffered with a fade-out and hold the rest for the late delivery.
        ++diagnostics.underruns;
        diagnostics.state = recovering;
        diagnostics.filteredErrorFrames = 0.0;
        int partial = std::min(frames, interpolators.front().getAvailableOutputSamples(ratio, available));
        diagnostics.correction = diagnostics.driftCorrection;
        compensation.store(diagnostics.correction, std::memory_order_relaxed);
        if (partial <= 0)
            return false;
        int consumed = renderFrames(dest, channels, partial, ratio, available);
        fifo.advanceRead(consumed);
        consumedFrames += consumed;
        FadeIn(dest, channels, partial, tuning.fadeFrames, &fadePosition);
        FadeOut(dest, channels, partial, tuning.fadeFrames);
        fadePosition = 0;
        return true;
    }

    int consumed = renderFrames(dest, channels, frames, ratio, need);
    FadeIn(dest, channels, frames, tuning.fadeFrames, &fadePosition);
    double excess = level - target;
    if (excess > std::max(target, DROP_MINIMUM_SECONDS * writer.rate))
    {
        // Far more than the reserve needs: skip it in one faded step instead of resampling for seconds.
        int dropped = static_cast<int>(std::min(excess, static_cast<double>(available - consumed)));
        FadeOut(dest, channels, frames, tuning.fadeFrames);
        fifo.advanceRead(consumed + dropped);
        consumedFrames += consumed + dropped;
        meanLevel = dipLevel - dropped;
        for (auto& interpolator : interpolators)
            interpolator.reset();
        fadePosition = 0;
        diagnostics.filteredErrorFrames = 0.0;
        ++diagnostics.drops;
    }
    else
    {
        fifo.advanceRead(consumed);
        consumedFrames += consumed;
        updateCorrection(excess, frames / reader.rate, learnDrift);
    }
    compensation.store(diagnostics.correction, std::memory_order_relaxed);
    return true;
}
