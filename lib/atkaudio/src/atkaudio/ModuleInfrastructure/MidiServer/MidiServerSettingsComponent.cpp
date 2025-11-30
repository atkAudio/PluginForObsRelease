#include "MidiServerSettingsComponent.h"

namespace atk
{

MidiServerSettingsComponent::MidiServerSettingsComponent(MidiClient* client)
    : client(client)
    , server(MidiServer::getInstance())
{
    DBG("[MIDI_SRV] MidiServerSettingsComponent created with client: " << (client ? "YES" : "NO"));

    // Input devices section
    inputsLabel.setText("MIDI Inputs", juce::dontSendNotification);
    inputsLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(inputsLabel);

    inputsViewport = std::make_unique<juce::Viewport>();
    inputsContainer = std::make_unique<juce::Component>();
    inputsViewport->setViewedComponent(inputsContainer.get(), false);
    inputsViewport->setScrollBarsShown(true, false); // Show vertical scrollbar, hide horizontal
    addAndMakeVisible(inputsViewport.get());

    // Output devices section
    outputsLabel.setText("MIDI Outputs", juce::dontSendNotification);
    outputsLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(outputsLabel);

    outputsViewport = std::make_unique<juce::Viewport>();
    outputsContainer = std::make_unique<juce::Component>();
    outputsViewport->setViewedComponent(outputsContainer.get(), false);
    outputsViewport->setScrollBarsShown(true, false); // Show vertical scrollbar, hide horizontal
    addAndMakeVisible(outputsViewport.get());

    // MIDI Keyboard
    keyboardLabel.setText("MIDI Keyboard", juce::dontSendNotification);
    keyboardLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(keyboardLabel);

    keyboardState = std::make_unique<juce::MidiKeyboardState>();
    keyboardComponent =
        std::make_unique<juce::MidiKeyboardComponent>(*keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard);
    addAndMakeVisible(keyboardComponent.get());

    // Use listener for immediate note callbacks (avoids polling delay that causes hanging notes)
    keyboardState->addListener(this);

    // MIDI Panic button
    panicButton = std::make_unique<juce::TextButton>("MIDI Reset");
    panicButton->onClick = [this] { sendMidiPanic(); };
    addAndMakeVisible(panicButton.get());

    // MIDI Monitor
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
    if (server != nullptr && client != nullptr)
    {
        auto state = server->getClientState(client);
        setSubscriptionState(state);
    }

    // Enable all MIDI inputs for monitoring
    if (server != nullptr)
    {
        auto& deviceManager = server->getAudioDeviceManager();
        auto midiInputs = juce::MidiInput::getAvailableDevices();
        for (const auto& device : midiInputs)
        {
            deviceManager.setMidiInputDeviceEnabled(device.identifier, true);
            deviceManager.addMidiInputDeviceCallback(device.identifier, this);
        }
    }

    // Start timer for updating monitor display
    startTimer(100);

    setSize(800, 600);
}

MidiServerSettingsComponent::~MidiServerSettingsComponent()
{
    stopTimer();

    // Remove keyboard listener
    if (keyboardState)
        keyboardState->removeListener(this);

    // Remove MIDI input callbacks
    if (server != nullptr)
    {
        auto& deviceManager = server->getAudioDeviceManager();
        auto midiInputs = juce::MidiInput::getAvailableDevices();
        for (const auto& device : midiInputs)
            deviceManager.removeMidiInputDeviceCallback(device.identifier, this);
    }
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

    // Monitor section (remaining space)
    monitorLabel.setBounds(bounds.removeFromTop(25));
    monitorTextEditor->setBounds(bounds);
}

MidiClientState MidiServerSettingsComponent::getSubscriptionState() const
{
    MidiClientState state;

    for (int i = 0; i < inputToggles.size(); ++i)
    {
        if (inputToggles[i]->getToggleState())
        {
            state.subscribedInputDevices.add(inputToggles[i]->getButtonText());
            DBG("[MIDI_SRV] Input device checked: " << inputToggles[i]->getButtonText());
        }
    }

    for (int i = 0; i < outputToggles.size(); ++i)
    {
        if (outputToggles[i]->getToggleState())
        {
            state.subscribedOutputDevices.add(outputToggles[i]->getButtonText());
            DBG("[MIDI_SRV] Output device checked: " << outputToggles[i]->getButtonText());
        }
    }

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

    // Clear existing toggles
    inputToggles.clear();
    outputToggles.clear();

    // Create input device toggles
    auto inputDevices = server->getAvailableMidiInputDevices();
    int y = 0;
    for (const auto& device : inputDevices)
    {
        auto* toggle = inputToggles.add(new juce::ToggleButton(device));
        toggle->setBounds(5, y, 300, 24);
        toggle->onClick = [this] { updateSubscriptions(); };
        inputsContainer->addAndMakeVisible(toggle);
        y += 26;
    }
    // Set container size to fit all toggles (enables scrolling when content exceeds viewport)
    inputsContainer->setSize(320, y > 0 ? y : 50);

    // Create output device toggles
    auto outputDevices = server->getAvailableMidiOutputDevices();
    y = 0;
    for (const auto& device : outputDevices)
    {
        auto* toggle = outputToggles.add(new juce::ToggleButton(device));
        toggle->setBounds(5, y, 300, 24);
        toggle->onClick = [this] { updateSubscriptions(); };
        outputsContainer->addAndMakeVisible(toggle);
        y += 26;
    }
    // Set container size to fit all toggles (enables scrolling when content exceeds viewport)
    outputsContainer->setSize(320, y > 0 ? y : 50);
}

void MidiServerSettingsComponent::updateSubscriptions()
{
    DBG("[MIDI_SRV] updateSubscriptions called - server: "
        << (server ? "YES" : "NO")
        << ", client: "
        << (client ? "YES" : "NO")
        << ", client ID: "
        << juce::String::toHexString((juce::pointer_sized_int)client->getClientId()));

    if (server == nullptr || client == nullptr)
        return;

    auto state = getSubscriptionState();

    DBG("[MIDI_SRV] getSubscriptionState returned "
        << state.subscribedInputDevices.size()
        << " inputs, "
        << state.subscribedOutputDevices.size()
        << " outputs");

    client->setSubscriptions(state);
}

void MidiServerSettingsComponent::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    // Only show messages from subscribed devices
    if (client != nullptr && server != nullptr)
    {
        auto state = server->getClientState(client);
        if (!state.subscribedInputDevices.contains(source->getName()))
            return; // Not subscribed to this device
    }

    // Add to keyboard state if it's a note on/off
    if (message.isNoteOnOrOff())
        keyboardState->processNextMidiEvent(message);

    // Format message for monitor
    juce::String messageText;
    messageText << source->getName() << ": ";

    if (message.isNoteOn())
        messageText
            << "Note On: "
            << juce::MidiMessage::getMidiNoteName(message.getNoteNumber(), true, true, 3)
            << " Vel: "
            << message.getVelocity();
    else if (message.isNoteOff())
        messageText << "Note Off: " << juce::MidiMessage::getMidiNoteName(message.getNoteNumber(), true, true, 3);
    else if (message.isController())
        messageText << "CC " << message.getControllerNumber() << ": " << message.getControllerValue();
    else if (message.isProgramChange())
        messageText << "Program Change: " << message.getProgramChangeNumber();
    else if (message.isPitchWheel())
        messageText << "Pitch Wheel: " << message.getPitchWheelValue();
    else if (message.isAftertouch())
        messageText << "Aftertouch: " << message.getAfterTouchValue();
    else if (message.isChannelPressure())
        messageText << "Channel Pressure: " << message.getChannelPressureValue();
    else
        messageText << "Other MIDI Message";

    // Add to pending messages (will be displayed in timer callback)
    juce::ScopedLock lock(monitorMutex);
    pendingMonitorMessages.add(messageText);
}

void MidiServerSettingsComponent::timerCallback()
{
    // Timer is now only used for updating the monitor display
    // Virtual keyboard events are handled immediately via handleNoteOn/handleNoteOff listeners

    // Update monitor display
    juce::ScopedLock lock(monitorMutex);

    if (pendingMonitorMessages.isEmpty())
        return;

    juce::String currentText = monitorTextEditor->getText();
    juce::StringArray lines = juce::StringArray::fromLines(currentText);

    // Add new messages
    for (const auto& msg : pendingMonitorMessages)
        lines.add(msg);

    // Limit number of lines
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

    // Immediately inject note-on into the MIDI client
    auto message = juce::MidiMessage::noteOn(midiChannel, midiNoteNumber, velocity);
    juce::MidiBuffer buffer;
    buffer.addEvent(message, 0);
    client->injectMidi(buffer);

    // Add to monitor display
    juce::String messageText;
    messageText
        << "Virtual Keyboard: Note On: "
        << juce::MidiMessage::getMidiNoteName(midiNoteNumber, true, true, 4)
        << " Vel: "
        << static_cast<int>(velocity * 127.0f);

    juce::ScopedLock lock(monitorMutex);
    pendingMonitorMessages.add(messageText);
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

    // Immediately inject note-off into the MIDI client
    auto message = juce::MidiMessage::noteOff(midiChannel, midiNoteNumber, velocity);
    juce::MidiBuffer buffer;
    buffer.addEvent(message, 0);
    client->injectMidi(buffer);

    // Add to monitor display
    juce::String messageText;
    messageText << "Virtual Keyboard: Note Off: " << juce::MidiMessage::getMidiNoteName(midiNoteNumber, true, true, 4);

    juce::ScopedLock lock(monitorMutex);
    pendingMonitorMessages.add(messageText);
}

void MidiServerSettingsComponent::sendMidiPanic()
{
    if (!server || !client)
        return;

    DBG("[MIDI_SRV] Sending MIDI Panic");

    juce::MidiBuffer panicMessages;

    // Send All Notes Off (CC 123) and All Controllers Off (CC 121) on all 16 channels
    for (int channel = 1; channel <= 16; ++channel)
    {
        // All Sound Off (CC 120)
        panicMessages.addEvent(juce::MidiMessage::controllerEvent(channel, 120, 0), 0);

        // All Controllers Off (CC 121)
        panicMessages.addEvent(juce::MidiMessage::controllerEvent(channel, 121, 0), 0);

        // All Notes Off (CC 123)
        panicMessages.addEvent(juce::MidiMessage::controllerEvent(channel, 123, 0), 0);
    }

    // Inject panic messages to the client
    server->injectMidiToClient(client, panicMessages);

    // Also clear the virtual keyboard state
    if (keyboardState)
        keyboardState->allNotesOff(1);

    DBG("[MIDI_SRV] MIDI Panic sent - all notes and controllers off on all channels");
}

} // namespace atk
