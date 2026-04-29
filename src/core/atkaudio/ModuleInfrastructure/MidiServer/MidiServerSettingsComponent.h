#pragma once

#include "MidiServer.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace atk
{

class MidiServerSettingsComponent
    : public juce::Component
    , private juce::MidiInputCallback
    , private juce::MidiKeyboardState::Listener
    , private juce::Timer
{
public:
    explicit MidiServerSettingsComponent(MidiClient* client);
    ~MidiServerSettingsComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    MidiClientState getSubscriptionState() const;

    void setSubscriptionState(const MidiClientState& state);

private:
    void updateDeviceLists();
    void updateSubscriptions();
    void sendMidiPanic();

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

    void handleNoteOn(juce::MidiKeyboardState* source, int midiChannel, int midiNoteNumber, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState* source, int midiChannel, int midiNoteNumber, float velocity) override;

    void timerCallback() override;

    MidiClient* client;
    MidiServer* server;

    juce::Label inputsLabel;
    std::unique_ptr<juce::Viewport> inputsViewport;
    std::unique_ptr<juce::Component> inputsContainer;
    juce::OwnedArray<juce::ToggleButton> inputToggles;

    juce::Label outputsLabel;
    std::unique_ptr<juce::Viewport> outputsViewport;
    std::unique_ptr<juce::Component> outputsContainer;
    juce::OwnedArray<juce::ToggleButton> outputToggles;

    juce::Label keyboardLabel;
    std::unique_ptr<juce::MidiKeyboardState> keyboardState;
    std::unique_ptr<juce::MidiKeyboardComponent> keyboardComponent;
    std::unique_ptr<juce::TextButton> panicButton;

    juce::Label monitorLabel;
    std::unique_ptr<juce::TextEditor> monitorTextEditor;

    juce::CriticalSection monitorMutex;
    juce::StringArray pendingMonitorMessages;
    int maxMonitorLines = 100;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiServerSettingsComponent)
};

} // namespace atk
