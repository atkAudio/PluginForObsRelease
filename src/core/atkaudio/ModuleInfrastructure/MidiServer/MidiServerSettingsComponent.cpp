#include "MidiServerSettingsComponent.h"

#include <atkaudio/Logging.h>

namespace atk
{

namespace
{
juce::String formatMonitorMessage(const MidiInputEvent& event)
{
    juce::String messageText;
    messageText << event.inputDeviceName << ": ";

    if (event.message.isNoteOn())
        messageText
            << "Note On: "
            << juce::MidiMessage::getMidiNoteName(event.message.getNoteNumber(), true, true, 3)
            << " Vel: "
            << event.message.getVelocity();
    else if (event.message.isNoteOff())
        messageText << "Note Off: " << juce::MidiMessage::getMidiNoteName(event.message.getNoteNumber(), true, true, 3);
    else if (event.message.isController())
        messageText << "CC " << event.message.getControllerNumber() << ": " << event.message.getControllerValue();
    else if (event.message.isProgramChange())
        messageText << "Program Change: " << event.message.getProgramChangeNumber();
    else if (event.message.isPitchWheel())
        messageText << "Pitch Wheel: " << event.message.getPitchWheelValue();
    else if (event.message.isAftertouch())
        messageText << "Aftertouch: " << event.message.getAfterTouchValue();
    else if (event.message.isChannelPressure())
        messageText << "Channel Pressure: " << event.message.getChannelPressureValue();
    else
        messageText << "Other MIDI Message";

    return messageText;
}
} // namespace

MidiServerSettingsComponent::MidiServerSettingsComponent(MidiClient* client)
    : client(client)
    , server(MidiServer::getInstance())
{
    atk::logging::info("MidiServerSettingsComponent::ctor", "initializing MIDI settings UI");
    // Input devices section
    inputsLabel.setText("MIDI Inputs", juce::dontSendNotification);
    inputsLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(inputsLabel);

    inputsViewport = std::make_unique<juce::Viewport>();
    inputsContainer = std::make_unique<juce::Component>();
    inputsViewport->setViewedComponent(inputsContainer.get(), false);
    inputsViewport->setScrollBarsShown(true, false);
    addAndMakeVisible(inputsViewport.get());

    // Output devices section
    outputsLabel.setText("MIDI Outputs", juce::dontSendNotification);
    outputsLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(outputsLabel);

    outputsViewport = std::make_unique<juce::Viewport>();
    outputsContainer = std::make_unique<juce::Component>();
    outputsViewport->setViewedComponent(outputsContainer.get(), false);
    outputsViewport->setScrollBarsShown(true, false);
    addAndMakeVisible(outputsViewport.get());

    // MIDI Keyboard
    keyboardLabel.setText("MIDI Keyboard", juce::dontSendNotification);
    keyboardLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(keyboardLabel);

    keyboardState = std::make_unique<juce::MidiKeyboardState>();
    keyboardComponent =
        std::make_unique<juce::MidiKeyboardComponent>(*keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard);
    addAndMakeVisible(keyboardComponent.get());

    keyboardState->addListener(this);

    // MIDI Panic button
    panicButton = std::make_unique<juce::TextButton>("MIDI Reset");
    panicButton->onClick = [this]
    {
        sendMidiPanic();
    };
    addAndMakeVisible(panicButton.get());

    monitorLabel.setText("MIDI Monitor", juce::dontSendNotification);
    monitorLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(monitorLabel);

    monitorTextEditor = std::make_unique<juce::TextEditor>();
    monitorTextEditor->setMultiLine(true);
    monitorTextEditor->setReadOnly(true);
    monitorTextEditor->setScrollbarsShown(true);
    monitorTextEditor->setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    addAndMakeVisible(monitorTextEditor.get());

    // Update device lists
    updateDeviceLists();

    // Load current subscription state
    if (client != nullptr)
    {
        auto state = client->getSubscriptions();
        setSubscriptionState(state);
    }

    startTimer(100);

    setSize(800, 600);
}

MidiServerSettingsComponent::~MidiServerSettingsComponent()
{
    stopTimer();

    // Remove keyboard listener
    if (keyboardState)
        keyboardState->removeListener(this);
}

void MidiServerSettingsComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void MidiServerSettingsComponent::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    // Top section: Inputs and Outputs side by side
    auto topSection = bounds.removeFromTop(200);
    auto inputSection = topSection.removeFromLeft(topSection.getWidth() / 2).reduced(5);
    auto outputSection = topSection.reduced(5);

    // Input devices
    inputsLabel.setBounds(inputSection.removeFromTop(25));
    inputsViewport->setBounds(inputSection);

    // Output devices
    outputsLabel.setBounds(outputSection.removeFromTop(25));
    outputsViewport->setBounds(outputSection);

    bounds.removeFromTop(10);

    // Keyboard section
    auto keyboardSection = bounds.removeFromTop(100);
    keyboardLabel.setBounds(keyboardSection.removeFromTop(25));

    // Panic button on the right side of keyboard
    auto panicButtonBounds = keyboardSection.removeFromRight(100).reduced(5);
    panicButton->setBounds(panicButtonBounds);

    keyboardComponent->setBounds(keyboardSection);

    bounds.removeFromTop(10);

    monitorLabel.setBounds(bounds.removeFromTop(25));
    monitorTextEditor->setBounds(bounds);
}

MidiClientState MidiServerSettingsComponent::getSubscriptionState() const
{
    MidiClientState state;

    for (int i = 0; i < inputToggles.size(); ++i)
        if (inputToggles[i]->getToggleState())
            state.subscribedInputDevices.add(inputToggles[i]->getButtonText());

    for (int i = 0; i < outputToggles.size(); ++i)
        if (outputToggles[i]->getToggleState())
            state.subscribedOutputDevices.add(outputToggles[i]->getButtonText());

    return state;
}

void MidiServerSettingsComponent::setSubscriptionState(const MidiClientState& state)
{
    for (auto* toggle : inputToggles)
    {
        toggle->setToggleState(
            state.subscribedInputDevices.contains(toggle->getButtonText()),
            juce::dontSendNotification
        );
    }

    for (auto* toggle : outputToggles)
    {
        toggle->setToggleState(
            state.subscribedOutputDevices.contains(toggle->getButtonText()),
            juce::dontSendNotification
        );
    }
}

void MidiServerSettingsComponent::updateDeviceLists()
{
    if (server == nullptr)
        return;

    inputToggles.clear();
    outputToggles.clear();

    auto inputDevices = server->getAvailableMidiInputDevices();
    int y = 0;
    for (const auto& device : inputDevices)
    {
        auto* toggle = inputToggles.add(new juce::ToggleButton(device));
        toggle->setBounds(5, y, 300, 24);
        toggle->onClick = [this]
        {
            updateSubscriptions();
        };
        inputsContainer->addAndMakeVisible(toggle);
        y += 26;
    }
    inputsContainer->setSize(320, y > 0 ? y : 50);

    auto outputDevices = server->getAvailableMidiOutputDevices();
    y = 0;
    for (const auto& device : outputDevices)
    {
        auto* toggle = outputToggles.add(new juce::ToggleButton(device));
        toggle->setBounds(5, y, 300, 24);
        toggle->onClick = [this]
        {
            updateSubscriptions();
        };
        outputsContainer->addAndMakeVisible(toggle);
        y += 26;
    }
    outputsContainer->setSize(320, y > 0 ? y : 50);
}

void MidiServerSettingsComponent::updateSubscriptions()
{
    if (server == nullptr || client == nullptr)
        return;

    auto state = getSubscriptionState();

    atk::logging::debug(
        "MidiServerSettingsComponent::updateSubscriptions",
        juce::String::formatted(
            "applying MIDI subscriptions: inputs=%d outputs=%d",
            state.subscribedInputDevices.size(),
            state.subscribedOutputDevices.size()
        )
    );
    client->setSubscriptions(state);
}

void MidiServerSettingsComponent::timerCallback()
{
    if (server == nullptr || monitorTextEditor == nullptr)
        return;

    std::vector<MidiInputEvent> monitorEvents;
    server->getPendingMonitorMidiEvents(monitorEvents, 4096);

    if (monitorEvents.empty())
        return;

    for (const auto& event : monitorEvents)
        pendingMonitorMessages.add(formatMonitorMessage(event));

    juce::StringArray lines = juce::StringArray::fromLines(monitorTextEditor->getText());
    for (const auto& message : pendingMonitorMessages)
        lines.add(message);

    while (lines.size() > maxMonitorLines)
        lines.remove(0);

    monitorTextEditor->setText(lines.joinIntoString("\n"));
    monitorTextEditor->moveCaretToEnd();
    pendingMonitorMessages.clear();
}

void MidiServerSettingsComponent::handleNoteOn(
    juce::MidiKeyboardState* source,
    int midiChannel,
    int midiNoteNumber,
    float velocity
)
{
    juce::ignoreUnused(source);

    if (!client)
        return;

    auto message = juce::MidiMessage::noteOn(midiChannel, midiNoteNumber, velocity);
    juce::MidiBuffer buffer;
    buffer.addEvent(message, 0);
    client->injectMidi(buffer);
}

void MidiServerSettingsComponent::handleNoteOff(
    juce::MidiKeyboardState* source,
    int midiChannel,
    int midiNoteNumber,
    float velocity
)
{
    juce::ignoreUnused(source);

    if (!client)
        return;

    auto message = juce::MidiMessage::noteOff(midiChannel, midiNoteNumber, velocity);
    juce::MidiBuffer buffer;
    buffer.addEvent(message, 0);
    client->injectMidi(buffer);
}

void MidiServerSettingsComponent::sendMidiPanic()
{
    if (!client)
        return;

    atk::logging::info("MidiServerSettingsComponent::sendMidiPanic", "sending MIDI reset across all channels");
    juce::MidiBuffer panicMessages;

    for (int channel = 1; channel <= 16; ++channel)
    {
        panicMessages.addEvent(juce::MidiMessage::controllerEvent(channel, 120, 0), 0);
        panicMessages.addEvent(juce::MidiMessage::controllerEvent(channel, 121, 0), 0);
        panicMessages.addEvent(juce::MidiMessage::controllerEvent(channel, 123, 0), 0);
    }

    client->injectMidi(panicMessages);

    if (keyboardState)
        keyboardState->allNotesOff(1);

    atk::logging::info("MidiServerSettingsComponent::sendMidiPanic", "MIDI reset sent");
}

} // namespace atk
