#include "ChordAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace chord_analyzer
{
namespace
{
struct ChordType
{
    const char* suffix;
    int mask;
};

ChordType types[] = {
    {         "", 0x091},
    {        "m", 0x089},
    {      "dim", 0x049},
    {      "aug", 0x111},
    {     "sus2", 0x085},
    {     "sus4", 0x0a1},
    {        "6", 0x291},
    {       "m6", 0x289},
    {      "6/9", 0x295},
    {     "m6/9", 0x28d},
    {        "7", 0x491},
    {     "maj7", 0x891},
    {       "m7", 0x489},
    {  "m(maj7)", 0x889},
    {     "dim7", 0x249},
    {     "m7b5", 0x449},
    {     "aug7", 0x511},
    {"aug(maj7)", 0x911},
    {    "7sus2", 0x485},
    {    "7sus4", 0x4a1},
    {     "add9", 0x095},
    {  "m(add9)", 0x08d},
    {        "9", 0x495},
    {     "maj9", 0x895},
    {       "m9", 0x48d},
    {  "m(maj9)", 0x88d},
    {       "11", 0x4b5},
    {    "maj11", 0x8b5},
    {      "m11", 0x4ad},
    {       "13", 0x6b5},
    {    "maj13", 0xab5},
    {      "m13", 0x6ad}
};

int RelativeMask(int pitches, int root)
{
    return ((pitches >> root) | (pitches << (12 - root))) & 0xfff;
}
} // namespace

uint32_t Result::encode()
{
    return pitches | (root << 12) | (bass << 16) | (type << 23) | (status << 29);
}

Result Result::decode(uint32_t value)
{
    return {
        int(value & 0xfff),
        int((value >> 12) & 15),
        int((value >> 16) & 127),
        int((value >> 23) & 63),
        Status((value >> 29) & 3)
    };
}

int Result::vst3Mask()
{
    return status == recognized ? RelativeMask(pitches, root) >> 1 : 0;
}

Result Recognize(int pitches, int bass)
{
    Result result{pitches, 0, bass, 0, pitches == 0 ? silent : unknown};
    if (pitches == 0)
        return result;
    for (int candidate = 0; candidate < 12; ++candidate)
    {
        int root = (bass + candidate) % 12;
        if ((pitches & (1 << root)) == 0)
            continue;
        int mask = RelativeMask(pitches, root);
        for (int type = 0; type < int(std::size(types)); ++type)
            if (types[type].mask == mask)
                return {pitches, root, bass, type, recognized};
    }
    return result;
}

const char* Suffix(int type)
{
    return type >= 0 && type < int(std::size(types)) ? types[type].suffix : "";
}

const char* NoteName(int pitch)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[pitch % 12];
}

void Analyzer::prepare(double sampleRate)
{
    settleSamples = std::max(1, int(std::round(sampleRate * 0.030)));
    reset();
}

void Analyzer::reset()
{
    held = {};
    sustained = {};
    pedal = {};
    sounding = {};
    remaining = 0;
    result = {};
}

void Analyzer::advance(int samples)
{
    if (remaining <= 0)
        return;
    remaining -= samples;
    if (remaining <= 0)
        result = Recognize(result.pitches, result.bass);
}

void Analyzer::updateNote(int channel, int note, bool wasSounding)
{
    bool isSounding = held[channel][note] != 0 || sustained[channel][note];
    sounding[note] += int(isSounding) - int(wasSounding);
}

void Analyzer::refresh()
{
    int pitches = 0;
    int bass = 0;
    for (int note = 0; note < 128; ++note)
        if (sounding[note] != 0)
        {
            if (pitches == 0)
                bass = note;
            pitches |= 1 << (note % 12);
        }
    if (result.pitches == pitches && result.bass == bass)
        return;
    result = {pitches, 0, bass, 0, pitches == 0 ? silent : settling};
    remaining = pitches == 0 ? 0 : settleSamples;
}

void Analyzer::message(int channel, int command, int note, int value)
{
    if (channel < 0 || channel >= 16 || note < 0 || note >= 128)
        return;
    if (command == 0x90 || command == 0x80)
    {
        bool wasSounding = held[channel][note] != 0 || sustained[channel][note];
        if (command == 0x90 && value > 0)
        {
            if (held[channel][note] < std::numeric_limits<uint16_t>::max())
                ++held[channel][note];
            sustained[channel][note] = false;
        }
        else if (held[channel][note] != 0)
        {
            --held[channel][note];
            if (held[channel][note] == 0)
                sustained[channel][note] = pedal[channel];
        }
        updateNote(channel, note, wasSounding);
    }
    else if (command == 0xb0)
    {
        if (note == 64)
            pedal[channel] = value >= 64;
        else if (note == 121)
            pedal[channel] = false;
        else if (note != 120 && note != 123)
            return;
        for (int pitch = 0; pitch < 128; ++pitch)
        {
            bool wasSounding = held[channel][pitch] != 0 || sustained[channel][pitch];
            if (note == 120 || note == 123)
            {
                if (held[channel][pitch] != 0 && pedal[channel] && note == 123)
                    sustained[channel][pitch] = true;
                held[channel][pitch] = 0;
            }
            if (!pedal[channel] || note == 120)
                sustained[channel][pitch] = false;
            updateNote(channel, pitch, wasSounding);
        }
    }
    else
        return;
    refresh();
}

Result Analyzer::getResult()
{
    return result;
}

uint32_t PublishedChord::activate(bool send)
{
    uint32_t identifier = nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    enabled.store(send, std::memory_order_release);
    generation.store(identifier, std::memory_order_release);
    return identifier;
}

void PublishedChord::deactivate(uint32_t identifier)
{
    generation.compare_exchange_strong(identifier, 0, std::memory_order_acq_rel);
}

void PublishedChord::publish(uint32_t identifier, Result result)
{
    if (identifier != 0 && identifier == generation.load(std::memory_order_acquire))
        staged.store((uint64_t(identifier) << 32) | result.encode(), std::memory_order_release);
}

Result PublishedChord::read()
{
    uint32_t identifier = generation.load(std::memory_order_acquire);
    uint64_t value = staged.load(std::memory_order_acquire);
    if (identifier == 0
        || uint32_t(value >> 32) != identifier
        || !enabled.load(std::memory_order_acquire)
        || generation.load(std::memory_order_acquire) != identifier)
        return {};
    return Result::decode(uint32_t(value));
}
} // namespace chord_analyzer
