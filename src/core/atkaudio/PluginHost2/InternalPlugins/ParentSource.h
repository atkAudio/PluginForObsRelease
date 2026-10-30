#pragma once

#include "../ParentSourceBridge.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace atk
{
class ParentSourceProcessor : public juce::AudioPluginInstance
{
public:
    enum Role
    {
        input,
        output
    };

    ParentSourceProcessor(Role nodeRole);
    void setBridge(ParentSourceBridge* owner);
    Role getRole();
    static bool isParentDescription(juce::PluginDescription description);
    void fillInPluginDescription(juce::PluginDescription& description) const override;
    const juce::String getName() const override;
    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& samples, juce::MidiBuffer& midi) override;
    void processBlock(juce::AudioBuffer<float>& samples, juce::MidiBuffer& midi, double timeSeconds);
    bool isBusesLayoutSupported(const BusesLayout& layout) const override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override;
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

private:
    Role role;
    std::atomic<ParentSourceBridge*> bridge{nullptr};
    std::atomic<uint64_t> generation{0};
    double processingSampleRate = 48000.0;
};
} // namespace atk
