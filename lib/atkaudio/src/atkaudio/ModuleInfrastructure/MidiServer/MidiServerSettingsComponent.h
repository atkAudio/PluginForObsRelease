#pragma once

#include "MidiServer.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace atk
{

/**
 * Settings component for managing MIDI device subscriptions
 * Provides UI for toggling inputs/outputs, MIDI keyboard, and message monitoring
 */
class MidiServerSettingsComponent
    : public juce::Component
    , private juce::MidiInputCallback
    , private juce::Timer
{
public:
    /**
     * Create settings component for a specific MIDI client
     * @param client The client whose subscriptions to manage
     */
    explicit MidiServerSettingsComponent(MidiClient* client);
    ~MidiServerSettingsComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /**
     * Get the current subscription state from the UI
     */
    MidiClientState getSubscriptionState() const;

    /**
     * Set the subscription state in the UI
     */
    void setSubscriptionState(const MidiClientState& state);

private:
    void updateDeviceLists();
    void updateSubscriptions();
    void sendMidiPanic();

    // MidiInputCallback
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

    // Timer for monitoring display updates
    void timerCallback() override;

    MidiClient* client;
    MidiServer* server;

    // UI Components
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

    // Message monitoring
    juce::CriticalSection monitorMutex;
    juce::StringArray pendingMonitorMessages;
    int maxMonitorLines = 100;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiServerSettingsComponent)
};

} // namespace atk
