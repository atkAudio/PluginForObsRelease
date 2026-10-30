#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

struct MissingPlugin : juce::AudioPluginInstance
{
    MissingPlugin(
        const juce::XmlElement&,
        const juce::PluginDescription&,
        juce::String reason,
        bool midiInput,
        bool midiOutput
    );

    void fillInPluginDescription(juce::PluginDescription&) const override;
    const juce::String getName() const override;
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
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

    juce::XmlElement originalXml;
    juce::PluginDescription description;
    juce::String failureReason;

private:
    static BusesProperties savedBuses(const juce::XmlElement&, const juce::PluginDescription&);
    bool midiInput;
    bool midiOutput;
    BusesLayout savedLayout;
    juce::MemoryBlock state;
    std::vector<int> sourceChannels;
    juce::AudioBuffer<float> floatScratch;
    juce::AudioBuffer<double> doubleScratch;
};
