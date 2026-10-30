#pragma once

#include "../HostTransport.h"
#include <juce_audio_processors/juce_audio_processors.h>

class PluginGraph;

class TransportProcessor : public juce::AudioPluginInstance
{
public:
    TransportProcessor();
    ~TransportProcessor() override;
    static bool isTransportDescription(juce::PluginDescription);
    static bool readState(const void*, int, TransportSettings&, int& modes, double& position);
    void bind(PluginGraph*, bool enabled = true);
    bool command(TransportOperation, double value = 0.0);
    TransportSettings getSettings();
    bool setSettings(TransportSettings, bool continueTapSequence = false);
    int getModes();
    void setModes(int);
    double getQuarterNotes();
    juce::String getPositionText();
    bool seekPositionText(juce::String);
    bool isPlaying();
    bool isPaused();
    int getEffectiveTempo();
    int getSyncStatus();
    uint32_t getRejectedCommands();
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
    PluginGraph* graph = nullptr;
    std::atomic<PublishedTransport*> shared{nullptr};
    std::atomic<uint32_t> generation{0};
    TransportSettings savedSettings;
    int savedModes = continuousClock;
    double savedPosition = 0.0;
};
