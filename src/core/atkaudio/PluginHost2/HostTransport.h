#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <array>

struct TransportSettings
{
    int tempoHundredths = 12000;
    int numerator = 4;
    int denominator = 4;

    bool isValid() const;
    uint32_t encode() const;
    static TransportSettings decode(uint32_t);
};

enum TransportMode
{
    followClock = 1,
    followPosition = 2,
    followPlayback = 4,
    sendClock = 8,
    sendPlayback = 16,
    sendPosition = 32,
    continuousClock = 64
};

enum TransportOperation
{
    transportPlay,
    transportPause,
    transportStop,
    transportSeek,
    transportPreviousBar,
    transportNextBar,
    transportMidiClock,
    transportMidiStart,
    transportMidiContinue,
    transportMidiStop
};

struct TransportCommand
{
    uint32_t generation = 0;
    TransportOperation operation = transportPause;
    double value = 0.0;
};

class TransportCommandQueue
{
public:
    bool push(TransportCommand);
    bool pop(TransportCommand&);

private:
    juce::AbstractFifo fifo{256};
    std::array<TransportCommand, 256> commands{};
};

struct PublishedTransport
{
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<int64_t>::is_always_lock_free);
    std::atomic<uint32_t> value{TransportSettings{}.encode()};

    TransportSettings get() const;
    void set(TransportSettings);
    uint32_t activate(TransportSettings, int modes, double quarterNotes);
    void deactivate(uint32_t generation);
    bool request(TransportCommand, bool fromMidi = false);

    std::atomic<uint32_t> activeGeneration{0};
    std::atomic<uint32_t> nextGeneration{0};
    std::atomic<int> modes{continuousClock};
    std::atomic<double> restorePosition{0.0};
    std::atomic<double> quarterNotes{0.0};
    std::atomic<int> effectiveTempo{12000};
    std::atomic<bool> playing{true};
    std::atomic<bool> paused{false};
    std::atomic<int> syncStatus{0};
    std::atomic<int64_t> renderSample{0};
    std::atomic<uint32_t> rejectedCommands{0};
    TransportCommandQueue uiCommands;
    TransportCommandQueue midiCommands;
    juce::MidiBuffer generatedMidi;
};

class HostTransport : public juce::AudioPlayHead
{
public:
    void prepare(double sampleRateHz);
    void restart(double sampleRateHz);
    void beginBlock(TransportSettings);
    void beginBlock(PublishedTransport&, int numSamples);
    void endBlock(int numSamples);
    juce::Optional<PositionInfo> getPosition() const override;

private:
    double sampleRate = 44100.0;
    int64_t samples = 0;
    double quarterNotes = 0.0;
    double tempo = 120.0;
    double seconds = 0.0;
    bool playing = true;
    bool paused = false;
    int64_t renderSamples = 0;
    uint32_t generation = 0;
    int modes = continuousClock;
    double nextClockOffset = 0.0;
    double lastClockSample = -1.0;
    double clockQuarterNotes = 0.0;
    double clockIntervalTotal = 0.0;
    std::array<double, 24> clockIntervals{};
    int clockIntervalCount = 0;
    int clockIntervalIndex = 0;
    int syncStatus = 0;
    PositionInfo position;
    void updatePosition(TransportSettings);
    void applyCommand(TransportCommand, PublishedTransport&);
    void seek(double, PublishedTransport&);
    void emitPosition(PublishedTransport&);
    void emitPlayback(PublishedTransport&, bool rewind);
    void readClock(double sampleTime);
};

struct TapTempo
{
    bool press(double timeSeconds, int& tempoHundredths);
    void release();
    void reset();
    bool expired(double timeSeconds) const;

private:
    std::array<double, 4> intervals{};
    double lastTap = 0.0;
    int count = 0;
    int next = 0;
    bool started = false;
    bool held = false;
};
