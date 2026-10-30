#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace chord_analyzer
{
enum Status
{
    silent,
    settling,
    unknown,
    recognized
};

struct Result
{
    int pitches = 0;
    int root = 0;
    int bass = 0;
    int type = 0;
    Status status = silent;

    uint32_t encode();
    static Result decode(uint32_t);
    int vst3Mask();
};

Result Recognize(int pitches, int bass);
const char* Suffix(int type);
const char* NoteName(int pitch);

struct PublishedChord
{
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::atomic<uint32_t> generation{0};
    std::atomic<uint32_t> nextGeneration{0};
    std::atomic<uint64_t> staged{0};
    std::atomic<bool> enabled{true};

    uint32_t activate(bool send);
    void deactivate(uint32_t identifier);
    void publish(uint32_t identifier, Result);
    Result read();
};

class Analyzer
{
public:
    void prepare(double sampleRate);
    void reset();
    void advance(int samples);
    void message(int channel, int command, int note, int value);
    Result getResult();

private:
    std::array<std::array<uint16_t, 128>, 16> held{};
    std::array<std::array<bool, 128>, 16> sustained{};
    std::array<bool, 16> pedal{};
    std::array<int, 128> sounding{};
    int settleSamples = 1323;
    int remaining = 0;
    Result result;
    void refresh();
    void updateNote(int channel, int note, bool wasSounding);
};
} // namespace chord_analyzer
