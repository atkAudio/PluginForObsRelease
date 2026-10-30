#include "HostTransport.h"

bool TransportSettings::isValid() const
{
    return tempoHundredths >= 100
        && tempoHundredths <= 99900
        && numerator >= 1
        && numerator <= 32
        && denominator >= 1
        && denominator <= 64
        && (denominator & (denominator - 1)) == 0;
}

uint32_t TransportSettings::encode() const
{
    return static_cast<uint32_t>(tempoHundredths)
         | (static_cast<uint32_t>(numerator - 1) << 17)
         | (static_cast<uint32_t>(denominator) << 22);
}

TransportSettings TransportSettings::decode(uint32_t encoded)
{
    return {
        static_cast<int>(encoded & 0x1ffff),
        static_cast<int>((encoded >> 17) & 31) + 1,
        static_cast<int>(encoded >> 22)
    };
}

TransportSettings PublishedTransport::get() const
{
    return TransportSettings::decode(value.load(std::memory_order_acquire));
}

void PublishedTransport::set(TransportSettings settings)
{
    jassert(settings.isValid());
    value.store(settings.encode(), std::memory_order_release);
}

bool TransportCommandQueue::push(TransportCommand command)
{
    auto writer = fifo.write(1);
    if (writer.blockSize1 == 0)
        return false;
    commands[writer.startIndex1] = command;
    return true;
}

bool TransportCommandQueue::pop(TransportCommand& command)
{
    auto reader = fifo.read(1);
    if (reader.blockSize1 == 0)
        return false;
    command = commands[reader.startIndex1];
    return true;
}

uint32_t PublishedTransport::activate(TransportSettings settings, int requestedModes, double position)
{
    set(settings);
    modes.store(requestedModes, std::memory_order_relaxed);
    restorePosition.store(position, std::memory_order_relaxed);
    uint32_t identifier = nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    activeGeneration.store(identifier, std::memory_order_release);
    return identifier;
}

void PublishedTransport::deactivate(uint32_t identifier)
{
    activeGeneration.compare_exchange_strong(identifier, 0, std::memory_order_acq_rel);
}

bool PublishedTransport::request(TransportCommand command, bool fromMidi)
{
    if (command.generation == 0 || command.generation != activeGeneration.load(std::memory_order_acquire))
        return false;
    if ((fromMidi ? midiCommands : uiCommands).push(command))
        return true;
    rejectedCommands.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void HostTransport::prepare(double sampleRateHz)
{
    sampleRate = sampleRateHz;
    samples = 0;
    quarterNotes = 0.0;
    seconds = 0.0;
    playing = true;
    paused = false;
    generation = 0;
    renderSamples = 0;
    nextClockOffset = 0.0;
    lastClockSample = -1.0;
    clockIntervalCount = 0;
    clockIntervalIndex = 0;
    clockIntervalTotal = 0.0;
    syncStatus = 0;
    position = {};
}

void HostTransport::restart(double sampleRateHz)
{
    sampleRate = sampleRateHz;
    samples = std::llround(seconds * sampleRate);
    renderSamples = 0;
    nextClockOffset = 0.0;
    lastClockSample = -1.0;
    clockIntervalCount = 0;
    clockIntervalIndex = 0;
    clockIntervalTotal = 0.0;
    syncStatus = 0;
}

void HostTransport::beginBlock(TransportSettings settings)
{
    tempo = settings.tempoHundredths / 100.0;
    updatePosition(settings);
}

void HostTransport::updatePosition(TransportSettings settings)
{
    double quarterNotesPerBar = settings.numerator * 4.0 / settings.denominator;
    position.setTimeInSamples(samples);
    position.setTimeInSeconds(seconds);
    position.setBpm(tempo);
    position.setTimeSignature(TimeSignature{settings.numerator, settings.denominator});
    position.setPpqPosition(quarterNotes);
    position.setPpqPositionOfLastBarStart(std::floor(quarterNotes / quarterNotesPerBar) * quarterNotesPerBar);
    position.setIsPlaying(playing);
    position.setIsRecording(false);
    position.setIsLooping(false);
}

void HostTransport::endBlock(int numSamples)
{
    renderSamples += numSamples;
    if (playing)
    {
        samples += numSamples;
        seconds += numSamples / sampleRate;
        quarterNotes += numSamples * tempo / (60.0 * sampleRate);
    }
}

void HostTransport::emitPosition(PublishedTransport& shared)
{
    if ((modes & sendPosition) == 0)
        return;
    double songPosition = std::floor(quarterNotes * 4.0);
    if (songPosition > 16383.0)
    {
        shared.rejectedCommands.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    shared.generatedMidi.addEvent(juce::MidiMessage::songPositionPointer(static_cast<int>(songPosition)), 0);
}

void HostTransport::emitPlayback(PublishedTransport& shared, bool rewind)
{
    if ((modes & sendPlayback) == 0)
        return;
    shared.generatedMidi.addEvent(
        !playing ? juce::MidiMessage::midiStop()
        : rewind ? juce::MidiMessage::midiStart()
                 : juce::MidiMessage::midiContinue(),
        0
    );
}

void HostTransport::seek(double destination, PublishedTransport& shared)
{
    bool announce = playing && (modes & sendPosition) != 0;
    if (announce && (modes & sendPlayback) != 0)
        shared.generatedMidi.addEvent(juce::MidiMessage::midiStop(), 0);
    quarterNotes = juce::jlimit(0.0, 1000000000.0, destination);
    seconds = quarterNotes * 60.0 / tempo;
    samples = std::llround(seconds * sampleRate);
    clockQuarterNotes = quarterNotes;
    if (playing || (modes & continuousClock) == 0)
        nextClockOffset = 0.0;
    emitPosition(shared);
    if (announce)
        emitPlayback(shared, false);
}

void HostTransport::readClock(double sampleTime)
{
    double timeout =
        clockIntervalCount < 4 ? sampleRate * 8.0 : juce::jmax(sampleRate, sampleRate * 180.0 / (tempo * 24.0));
    if (lastClockSample < 0.0 || sampleTime - lastClockSample > timeout)
    {
        lastClockSample = sampleTime;
        clockQuarterNotes = quarterNotes - (renderSamples - sampleTime) * tempo / (60.0 * sampleRate);
        clockIntervalCount = 0;
        clockIntervalIndex = 0;
        clockIntervalTotal = 0.0;
        syncStatus = 1;
        return;
    }
    double interval = sampleTime - lastClockSample;
    if (interval < sampleRate * 60.0 / (999.0 * 24.0) || interval > sampleRate * 60.0 / 24.0)
        return;
    lastClockSample = sampleTime;
    if (clockIntervalCount == static_cast<int>(clockIntervals.size()))
        clockIntervalTotal -= clockIntervals[clockIntervalIndex];
    else
        ++clockIntervalCount;
    clockIntervals[clockIntervalIndex] = interval;
    clockIntervalTotal += interval;
    clockIntervalIndex = (clockIntervalIndex + 1) % static_cast<int>(clockIntervals.size());
    clockQuarterNotes += 1.0 / 24.0;
    if (clockIntervalCount >= 4)
    {
        tempo = 60.0 * sampleRate * clockIntervalCount / (24.0 * clockIntervalTotal);
        syncStatus = 2;
        if (playing)
        {
            double target = clockQuarterNotes + (renderSamples - sampleTime) * tempo / (60.0 * sampleRate);
            quarterNotes += juce::jlimit(-0.01, 0.01, (target - quarterNotes) * 0.1);
        }
        else
            clockQuarterNotes = quarterNotes - (renderSamples - sampleTime) * tempo / (60.0 * sampleRate);
    }
}

void HostTransport::applyCommand(TransportCommand command, PublishedTransport& shared)
{
    if (command.generation != generation)
        return;
    double barLength = shared.get().numerator * 4.0 / shared.get().denominator;
    switch (command.operation)
    {
    case transportMidiStart:
        if ((modes & followPlayback) != 0)
        {
            playing = false;
            seek(0.0, shared);
            playing = true;
            paused = false;
            nextClockOffset = 0.0;
            emitPlayback(shared, true);
        }
        break;
    case ::transportPlay:
    case transportMidiContinue:
        if (command.operation == transportMidiContinue && (modes & followPlayback) == 0)
            break;
        if (!playing)
        {
            playing = true;
            paused = false;
            emitPosition(shared);
            emitPlayback(shared, command.operation == ::transportPlay && quarterNotes == 0.0);
            if (quarterNotes == 0.0 || (modes & continuousClock) == 0)
                nextClockOffset = 0.0;
        }
        break;
    case transportPause:
    case transportMidiStop:
    case ::transportStop:
        if (command.operation == transportMidiStop && (modes & followPlayback) == 0)
            break;
        paused = command.operation != ::transportStop;
        if (playing)
        {
            playing = false;
            emitPlayback(shared, false);
        }
        if (command.operation == ::transportStop)
            seek(0.0, shared);
        break;
    case transportSeek:
        seek(command.value, shared);
        break;
    case transportPreviousBar:
        seek((std::floor(quarterNotes / barLength) - 1.0) * barLength, shared);
        break;
    case transportNextBar:
        seek((std::floor(quarterNotes / barLength) + 1.0) * barLength, shared);
        break;
    case transportMidiClock:
        if ((modes & followClock) != 0)
            readClock(command.value);
        break;
    }
}

void HostTransport::beginBlock(PublishedTransport& shared, int numSamples)
{
    shared.generatedMidi.clear();
    auto settings = shared.get();
    uint32_t active = shared.activeGeneration.load(std::memory_order_acquire);
    int requestedModes = active == 0 ? 0 : shared.modes.load(std::memory_order_acquire);
    double previousTempo = tempo;
    if (generation != active)
    {
        generation = active;
        playing = active == 0;
        paused = false;
        if (active != 0)
        {
            quarterNotes = shared.restorePosition.load(std::memory_order_relaxed);
            seconds = quarterNotes * 6000.0 / settings.tempoHundredths;
            samples = std::llround(seconds * sampleRate);
        }
        lastClockSample = -1.0;
        syncStatus = 0;
        nextClockOffset = 0.0;
    }
    if ((requestedModes & followClock) == 0 || (modes & followClock) == 0)
    {
        tempo = settings.tempoHundredths / 100.0;
        lastClockSample = -1.0;
        syncStatus = (requestedModes & followClock) != 0 ? 1 : 0;
    }
    bool resyncOutput = (requestedModes & ~modes & (sendPlayback | sendPosition)) != 0;
    if ((modes & sendPlayback) != 0 && (requestedModes & sendPlayback) == 0 && active != 0)
        shared.generatedMidi.addEvent(juce::MidiMessage::midiStop(), 0);
    modes = requestedModes;
    if (resyncOutput)
    {
        emitPosition(shared);
        emitPlayback(shared, quarterNotes == 0.0);
    }
    TransportCommand command;
    for (int index = 0; index < 256 && shared.uiCommands.pop(command); ++index)
        applyCommand(command, shared);
    for (int index = 0; index < 256 && shared.midiCommands.pop(command); ++index)
        applyCommand(command, shared);
    double timeout =
        clockIntervalCount < 4 ? sampleRate * 8.0 : juce::jmax(sampleRate, sampleRate * 180.0 / (tempo * 24.0));
    if ((modes & followClock) != 0 && lastClockSample >= 0.0 && renderSamples - lastClockSample > timeout)
        syncStatus = 3;
    if (previousTempo != tempo && previousTempo > 0.0)
        nextClockOffset *= previousTempo / tempo;
    if ((modes & followClock) != 0 && syncStatus == 2 && lastClockSample >= 0.0)
    {
        double period = sampleRate * 60.0 / (tempo * 24.0);
        double target = std::fmod(lastClockSample - renderSamples, period);
        if (target < 0.0)
            target += period;
        double error = target - nextClockOffset;
        if (error > period * 0.5)
            error -= period;
        if (error < -period * 0.5)
            error += period;
        nextClockOffset = juce::jmax(0.0, nextClockOffset + juce::jlimit(-period * 0.1, period * 0.1, error * 0.25));
    }
    if ((modes & sendClock) != 0 && (playing || (modes & continuousClock) != 0))
    {
        double samplesPerClock = sampleRate * 60.0 / (tempo * 24.0);
        int emitted = 0;
        while (nextClockOffset < numSamples && emitted < 4096)
        {
            shared.generatedMidi.addEvent(
                juce::MidiMessage::midiClock(),
                juce::jlimit(0, numSamples - 1, static_cast<int>(std::floor(nextClockOffset)))
            );
            nextClockOffset += samplesPerClock;
            ++emitted;
        }
        nextClockOffset -= numSamples;
        if (emitted == 4096)
        {
            nextClockOffset = 0.0;
            shared.rejectedCommands.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else
        nextClockOffset = 0.0;
    updatePosition(settings);
    shared.quarterNotes.store(quarterNotes, std::memory_order_relaxed);
    shared.playing.store(playing, std::memory_order_relaxed);
    shared.paused.store(paused, std::memory_order_relaxed);
    shared.effectiveTempo.store(juce::roundToInt(tempo * 100.0), std::memory_order_relaxed);
    shared.syncStatus.store(syncStatus, std::memory_order_relaxed);
    shared.renderSample.store(renderSamples, std::memory_order_release);
}

juce::Optional<juce::AudioPlayHead::PositionInfo> HostTransport::getPosition() const
{
    return position;
}

bool TapTempo::press(double timeSeconds, int& tempoHundredths)
{
    if (held)
        return false;
    held = true;

    if (!started || expired(timeSeconds))
    {
        count = 0;
        next = 0;
        started = true;
        lastTap = timeSeconds;
        return false;
    }

    double interval = timeSeconds - lastTap;
    if (interval < 60.0 / 999.0)
        return false;

    intervals[next] = interval;
    next = (next + 1) % static_cast<int>(intervals.size());
    count = juce::jmin(count + 1, static_cast<int>(intervals.size()));
    lastTap = timeSeconds;
    double total = 0.0;
    for (int index = 0; index < count; ++index)
        total += intervals[index];
    tempoHundredths = juce::roundToInt(6000.0 * count / total);
    return true;
}

void TapTempo::release()
{
    held = false;
}

void TapTempo::reset()
{
    *this = {};
}

bool TapTempo::expired(double timeSeconds) const
{
    return started && timeSeconds >= lastTap + 3.0;
}
