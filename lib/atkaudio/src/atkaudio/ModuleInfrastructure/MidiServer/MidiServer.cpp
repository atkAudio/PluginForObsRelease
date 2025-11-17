#include "MidiServer.h"

namespace atk
{

JUCE_IMPLEMENT_SINGLETON(MidiServer)

// ============================================================================
// MidiClientState
// ============================================================================

juce::String MidiClientState::serialize() const
{
    juce::XmlElement xml("MidiClientState");

    for (const auto& device : subscribedInputDevices)
        xml.createNewChildElement("InputDevice")->setAttribute("name", device);

    for (const auto& device : subscribedOutputDevices)
        xml.createNewChildElement("OutputDevice")->setAttribute("name", device);

    return xml.toString();
}

void MidiClientState::deserialize(const juce::String& data)
{
    subscribedInputDevices.clear();
    subscribedOutputDevices.clear();

    if (auto xml = juce::parseXML(data))
    {
        for (auto* child : xml->getChildIterator())
            if (child->hasTagName("InputDevice"))
                subscribedInputDevices.add(child->getStringAttribute("name"));
            else if (child->hasTagName("OutputDevice"))
                subscribedOutputDevices.add(child->getStringAttribute("name"));
    }
}

// ============================================================================
// MidiClient - Composition-based MIDI client
// ============================================================================

MidiClient::MidiClient(int queueSize)
    : clientId(this) // Use 'this' as unique identifier
{
    if (auto* server = MidiServer::getInstance())
        server->registerClient(clientId, MidiClientState(), queueSize);
}

MidiClient::~MidiClient()
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        server->unregisterClient(clientId);
}

MidiClient::MidiClient(MidiClient&& other) noexcept
    : clientId(other.clientId)
{
    other.clientId = nullptr;
}

MidiClient& MidiClient::operator=(MidiClient&& other) noexcept
{
    if (this != &other)
    {
        // Unregister old client ID
        if (clientId)
        {
            if (auto* server = MidiServer::getInstanceWithoutCreating())
                server->unregisterClient(clientId);
        }

        clientId = other.clientId;
        other.clientId = nullptr;
    }
    return *this;
}

void MidiClient::getPendingMidi(juce::MidiBuffer& outBuffer, int numSamples, double sampleRate)
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        server->getPendingMidiForClient(clientId, outBuffer, numSamples, sampleRate);
}

void MidiClient::sendMidi(const juce::MidiBuffer& messages)
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        server->sendMidiFromClient(clientId, messages);
}

void MidiClient::injectMidi(const juce::MidiBuffer& messages)
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        server->injectMidiToClient(clientId, messages);
}

void MidiClient::setSubscriptions(const MidiClientState& state)
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        server->updateClientSubscriptions(clientId, state);
}

MidiClientState MidiClient::getSubscriptions() const
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        return server->getClientState(clientId);
    return MidiClientState();
}

// ============================================================================
// MidiServer
// ============================================================================

MidiServer::MidiServer()
{
}

MidiServer::~MidiServer()
{
    shutdown();
    clearSingletonInstance();
}

void MidiServer::initialize()
{
    if (initialized)
        return;

    DBG("[MIDI_SRV] Initializing...");

    // Initialize AudioDeviceManager with MIDI only (no audio devices)
    juce::String error = deviceManager.initialise(
        0,       // numInputChannelsNeeded
        0,       // numOutputChannelsNeeded
        nullptr, // savedState
        true,    // selectDefaultDeviceOnFailure
        {},      // preferredDefaultDeviceName
        nullptr  // preferredSetupOptions
    );

    if (error.isNotEmpty())
    {
        DBG("[MIDI_SRV] Failed to initialize AudioDeviceManager: " + error);
        return;
    }

    // Enable all MIDI inputs by default (will be filtered per-client)
    auto midiInputs = juce::MidiInput::getAvailableDevices();
    for (const auto& device : midiInputs)
    {
        deviceManager.setMidiInputDeviceEnabled(device.identifier, true);
        deviceManager.addMidiInputDeviceCallback(device.identifier, this);
        DBG("[MIDI_SRV] Enabled MIDI input: " + device.name);
    }

    // Start timer for processing outgoing MIDI (10ms interval)
    startTimer(10);

    initialized = true;
    DBG("[MIDI_SRV] Initialized successfully");
}

void MidiServer::shutdown()
{
    if (!initialized)
        return;

    DBG("[MIDI_SRV] Shutting down...");

    // Stop timer first
    stopTimer();

    {
        juce::ScopedLock lock(clientsMutex);

        // Close all output devices
        for (juce::HashMap<juce::String, juce::MidiOutput*>::Iterator it(outputDevices); it.next();)
            if (it.getValue() != nullptr)
                delete it.getValue();
        outputDevices.clear();

        // Clear all clients (this destroys the lock-free queues)
        clients.clear();
    }

    // IMPORTANT: Explicitly disable all MIDI devices BEFORE AudioDeviceManager destructor
    // This prevents the crash from JUCE trying to start timers during shutdown
    auto midiInputs = juce::MidiInput::getAvailableDevices();
    for (const auto& device : midiInputs)
    {
        deviceManager.setMidiInputDeviceEnabled(device.identifier, false);
        deviceManager.removeMidiInputDeviceCallback(device.identifier, this);
    }

    // Close audio device (even though we're MIDI-only, this cleans up properly)
    deviceManager.closeAudioDevice();

    initialized = false;

    DBG("[MIDI_SRV] Shutdown complete");
}

void MidiServer::registerClient(void* clientId, const MidiClientState& state, int queueSize)
{
    if (!initialized || clientId == nullptr)
        return;

    juce::ScopedLock lock(clientsMutex);

    ClientInfo info(state, queueSize);
    clients.insert_or_assign(clientId, std::move(info));

    updateMidiDeviceSubscriptions();
    rebuildClientSnapshot();
}

void MidiServer::unregisterClient(void* clientId)
{
    if (!initialized || clientId == nullptr)
        return;

    juce::ScopedLock lock(clientsMutex);

    clients.erase(clientId);

    updateMidiDeviceSubscriptions();
    rebuildClientSnapshot();
}

void MidiServer::updateClientSubscriptions(void* clientId, const MidiClientState& state)
{
    if (!initialized || clientId == nullptr)
        return;

    juce::ScopedLock lock(clientsMutex);

    if (clients.contains(clientId))
    {
        clients.at(clientId).state = state;
        updateMidiDeviceSubscriptions();
        rebuildClientSnapshot();
    }
}

MidiClientState MidiServer::getClientState(void* clientId) const
{
    juce::ScopedLock lock(clientsMutex);

    auto it = clients.find(clientId);
    if (it != clients.end())
        return it->second.state;

    return MidiClientState();
}

void MidiServer::getPendingMidiForClient(void* clientId, juce::MidiBuffer& outBuffer, int numSamples, double sampleRate)
{
    if (!initialized)
        return;

    if (clientId == nullptr)
        return;

    // Lock only for reading the client info pointer
    // The queue operations themselves are lock-free
    ClientInfo* info = nullptr;

    {
        juce::ScopedLock lock(clientsMutex);
        if (!clients.contains(clientId))
            return;

        info = &clients.at(clientId);

        // Update client's sample rate if it changed
        if (info->sampleRate != sampleRate)
            info->sampleRate = sampleRate;
    }

    // Lock-free read from the queue - real-time safe!
    if (!info)
        return;

    if (!info->incomingMidiQueue)
        return;

    // Get all messages from queue within the current buffer size
    // The queue already stores sample positions, so we don't need timestamp conversion
    info->incomingMidiQueue->popAll(outBuffer, numSamples);
}

void MidiServer::sendMidiFromClient(void* clientId, const juce::MidiBuffer& messages)
{
    if (!initialized || clientId == nullptr)
        return;

    // Lock-free push to client's outgoing queue
    // The timer callback will process these messages
    ClientInfo* info = nullptr;
    {
        juce::ScopedLock lock(clientsMutex);
        if (!clients.contains(clientId))
            return;

        info = &clients.at(clientId);
    }

    // Push all messages to the lock-free queue
    for (const auto metadata : messages)
    {
        if (!info->outgoingMidiQueue->push(metadata.getMessage(), metadata.samplePosition))
        {
            // Queue full
            DBG("[MIDI_SRV] WARNING - Outgoing MIDI queue full, dropping message");
            break;
        }
    }
}

void MidiServer::injectMidiToClient(void* clientId, const juce::MidiBuffer& messages)
{
    if (!initialized || clientId == nullptr)
        return;

    // Lock-free push to client's incoming queue (as if from hardware input)
    ClientInfo* info = nullptr;
    {
        juce::ScopedLock lock(clientsMutex);
        if (!clients.contains(clientId))
            return;

        info = &clients.at(clientId);
    }

    // Push all messages to the lock-free queue
    for (const auto metadata : messages)
    {
        if (!info->incomingMidiQueue->push(metadata.getMessage(), metadata.samplePosition))
        {
            // Queue full
            DBG("[MIDI_SRV] WARNING - Incoming MIDI queue full, dropping injected message");
            break;
        }
    }
}

juce::StringArray MidiServer::getAvailableMidiInputDevices() const
{
    juce::StringArray devices;
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        devices.add(device.name);
    return devices;
}

juce::StringArray MidiServer::getAvailableMidiOutputDevices() const
{
    juce::StringArray devices;
    for (const auto& device : juce::MidiOutput::getAvailableDevices())
        devices.add(device.name);
    return devices;
}

void MidiServer::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    if (!initialized || source == nullptr)
        return;

    juce::String sourceName = source->getName();

    // REAL-TIME SAFE: Load snapshot atomically without blocking
    auto snapshot = activeSnapshot.load(std::memory_order_acquire);
    if (!snapshot)
        return;

    // Find clients subscribed to this input device
    auto it = snapshot->inputSubscriptions.find(sourceName);
    if (it == snapshot->inputSubscriptions.end())
        return;

    // Push to all subscribed clients' queues (lock-free)
    for (const auto& clientSnapshot : it->second)
    {
        if (clientSnapshot.incomingMidiQueue)
        {
            // Push with sample position 0 - MIDI from hardware arrives asynchronously
            // and will be processed at the start of the next audio callback
            if (!clientSnapshot.incomingMidiQueue->push(message, 0))
            {
                // Queue full - this should rarely happen with 65536 slots
                DBG("[MIDI_SRV] WARNING - Client MIDI queue full, dropping message");
            }
        }
    }
}

void MidiServer::timerCallback()
{
    if (!initialized)
        return;

    // Process outgoing MIDI from all clients
    // This runs on the message thread at 10ms intervals

    juce::Array<void*> clientList;
    {
        juce::ScopedLock lock(clientsMutex);

        // Collect client IDs
        for (auto& [clientId, info] : clients)
            clientList.add(clientId);
    }

    // Process each client's outgoing queue
    for (auto* clientId : clientList)
    {
        ClientInfo* info = nullptr;
        juce::StringArray outputDeviceNames;

        {
            juce::ScopedLock lock(clientsMutex);
            if (!clients.contains(clientId))
                continue;

            info = &clients.at(clientId);
            outputDeviceNames = info->state.subscribedOutputDevices;
        }

        if (!info || !info->outgoingMidiQueue)
            continue;

        // Read all pending messages from the lock-free queue
        // Use INT_MAX to avoid filtering messages by sample position
        juce::MidiBuffer outgoingMessages;
        info->outgoingMidiQueue->popAll(outgoingMessages, INT_MAX);

        if (outgoingMessages.isEmpty())
            continue;

        // Send to all subscribed output devices
        for (const auto& deviceName : outputDeviceNames)
        {
            juce::MidiOutput* output = nullptr;

            {
                juce::ScopedLock lock(clientsMutex);
                output = outputDevices[deviceName];

                if (output == nullptr)
                {
                    // Try to open the device if not already open
                    auto devices = juce::MidiOutput::getAvailableDevices();
                    for (const auto& device : devices)
                    {
                        if (device.name == deviceName)
                        {
                            output = juce::MidiOutput::openDevice(device.identifier).release();
                            if (output != nullptr)
                            {
                                outputDevices.set(deviceName, output);
                                DBG("[MIDI_SRV] Opened MIDI output: " + deviceName);
                            }
                            break;
                        }
                    }
                }
            }

            if (output != nullptr)
            {
                // Send all messages to the hardware output
                for (const auto metadata : outgoingMessages)
                    output->sendMessageNow(metadata.getMessage());
            }
        }
    }
}

void MidiServer::rebuildClientSnapshot()
{
    // Must be called while holding clientsMutex
    auto newSnapshot = std::make_shared<DeviceSnapshot>();

    // Build device -> clients mapping for fast lookup in MIDI callback
    for (auto& [clientId, info] : clients)
    {
        // For each input device this client subscribes to
        for (const auto& deviceName : info.state.subscribedInputDevices)
        {
            ClientSnapshot snapshot;
            // Share the queue pointers (they're thread-safe SPSC queues)
            snapshot.incomingMidiQueue = info.incomingMidiQueue;
            snapshot.outgoingMidiQueue = info.outgoingMidiQueue;
            snapshot.state = info.state;

            newSnapshot->inputSubscriptions[deviceName].push_back(std::move(snapshot));
        }

        // For each output device this client subscribes to
        for (const auto& deviceName : info.state.subscribedOutputDevices)
        {
            ClientSnapshot snapshot;
            snapshot.incomingMidiQueue = info.incomingMidiQueue;
            snapshot.outgoingMidiQueue = info.outgoingMidiQueue;
            snapshot.state = info.state;

            newSnapshot->outputSubscriptions[deviceName].push_back(std::move(snapshot));
        }
    }

    // Atomic publish - MIDI callback can now see new snapshot
    activeSnapshot.store(newSnapshot, std::memory_order_release);

    DBG("[MIDI_SRV] Rebuilt client snapshot: "
        + juce::String(newSnapshot->inputSubscriptions.size())
        + " input devices, "
        + juce::String(newSnapshot->outputSubscriptions.size())
        + " output devices");
}

void MidiServer::updateMidiDeviceSubscriptions()
{
    // Collect all unique device names that at least one client needs
    juce::StringArray neededInputs;
    juce::StringArray neededOutputs;

    for (auto& [clientPtr, info] : clients)
    {
        for (const auto& device : info.state.subscribedInputDevices)
            neededInputs.addIfNotAlreadyThere(device);

        for (const auto& device : info.state.subscribedOutputDevices)
            neededOutputs.addIfNotAlreadyThere(device);
    }

    // Close output devices that are no longer needed
    juce::Array<juce::String> devicesToRemove;
    for (juce::HashMap<juce::String, juce::MidiOutput*>::Iterator it(outputDevices); it.next();)
    {
        if (!neededOutputs.contains(it.getKey()))
        {
            if (it.getValue() != nullptr)
            {
                delete it.getValue();
                DBG("[MIDI_SRV] Closed MIDI output: " + it.getKey());
            }
            devicesToRemove.add(it.getKey());
        }
    }

    for (const auto& device : devicesToRemove)
        outputDevices.remove(device);
}

} // namespace atk
