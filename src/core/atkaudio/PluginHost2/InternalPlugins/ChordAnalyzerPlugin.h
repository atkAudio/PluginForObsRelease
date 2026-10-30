#pragma once

#include "ChordAnalyzer.h"
#include <juce_audio_processors/juce_audio_processors.h>

class ChordAnalyzerPlugin : public juce::AudioPluginInstance
{
public:
    ChordAnalyzerPlugin();
    ~ChordAnalyzerPlugin() override;
    static bool isDescription(juce::PluginDescription);
    void bind(std::shared_ptr<chord_analyzer::PublishedChord>, bool enabled);
    chord_analyzer::Result getResult();
    bool getSendChordContext();
    void setSendChordContext(bool);
    void fillInPluginDescription(juce::PluginDescription&) const override;
    const juce::String getName() const override;
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
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
    chord_analyzer::Analyzer analyzer;
    std::shared_ptr<chord_analyzer::PublishedChord> contextLifetime;
    std::atomic<chord_analyzer::PublishedChord*> context{nullptr};
    std::atomic<uint32_t> generation{0};
    uint32_t processingGeneration = 0;
    std::atomic<uint32_t> display{0};
    std::atomic<bool> sendChordContext{true};
};
