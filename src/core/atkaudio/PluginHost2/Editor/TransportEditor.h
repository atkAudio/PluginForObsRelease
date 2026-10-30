#pragma once

#include "../InternalPlugins/Transport.h"

class TransportEditor
    : public juce::AudioProcessorEditor
    , private juce::Timer
{
public:
    explicit TransportEditor(TransportProcessor&);
    ~TransportEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct TempoEditor;
    TransportProcessor& transport;
    std::unique_ptr<TempoEditor> bpm;
    juce::TextEditor numerator;
    juce::ComboBox denominator;
    juce::TextEditor position;
    juce::Label bpmLabel, signatureLabel, slash, positionLabel, inputLabel, outputLabel, status;
    juce::Label tapHint;
    std::array<std::unique_ptr<juce::ShapeButton>, 5> buttons;
    std::array<juce::ToggleButton, 7> switches;
    juce::TooltipWindow tooltips{this};
    TapTempo taps;
    bool tapHasEdit = false;
    uint32_t observedSettings = 0;
    int observedModes = 0;
    void timerCallback() override;
    void refresh(bool force = false);
    void resetTaps();
    void tap();
    void commitTempo();
    void commitNumerator();
};
