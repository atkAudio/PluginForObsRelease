#pragma once

#include <atkaudio/FifoBuffer2.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace atk
{
class ParentSourceBridge : public juce::ChangeBroadcaster
{
public:
    enum Mode
    {
        stopped,
        obsAudio,
        hardware
    };

    void start(Mode newMode, int channels, int blockSize, double sampleRate);
    void stop();
    void reset();
    Mode getMode();
    int getParentChannels();
    uint64_t getGeneration();
    bool processParent(float* const* samples, int channels, int frames, double sampleRate);
    bool processParent(float* const* samples, int channels, int frames, double sampleRate, double timeSeconds);
    void readInput(juce::AudioBuffer<float>& samples, double sampleRate);
    void readInput(juce::AudioBuffer<float>& samples, double sampleRate, double timeSeconds);
    void readInput(juce::AudioBuffer<float>& samples, double sampleRate, double timeSeconds, uint64_t generation);
    void writeOutput(juce::AudioBuffer<float>& samples, double sampleRate);
    void writeOutput(juce::AudioBuffer<float>& samples, double sampleRate, double timeSeconds);
    void writeOutput(juce::AudioBuffer<float>& samples, double sampleRate, double timeSeconds, uint64_t generation);

private:
    void prepareBuffers();

    std::shared_mutex lifecycleLock;
    std::atomic<Mode> mode{stopped};
    std::atomic<int> parentChannels{2};
    std::atomic<uint64_t> generation{0};
    double parentSampleRate = 0.0;
    SyncBuffer input{"Parent Source In"};
    SyncBuffer output{"Parent Source Out"};
    int preparedChannels = 2;
    int preparedBlockSize = 512;
    double preparedSampleRate = 48000.0;
};
} // namespace atk
