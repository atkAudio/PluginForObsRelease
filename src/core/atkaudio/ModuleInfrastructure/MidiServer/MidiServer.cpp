#include "MidiServer.h"

namespace atk
{

JUCE_IMPLEMENT_SINGLETON(MidiServer)

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

MidiClient::MidiClient(int queueSize)
    : clientId(this)
    , incomingQueue(std::make_shared<MidiMessageQueue>(queueSize))
    , outgoingQueue(std::make_shared<MidiMessageQueue>(queueSize))
{
    if (auto* server = MidiServer::getInstance())
    {
        auto incoming = incomingQueue.load(std::memory_order_acquire);
        auto outgoing = outgoingQueue.load(std::memory_order_acquire);
        server->registerClient(clientId, MidiClientState(), queueSize, incoming, outgoing);
    }
}

MidiClient::~MidiClient()
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        server->unregisterClient(clientId);
}

MidiClient::MidiClient(MidiClient&& other) noexcept
    : clientId(other.clientId)
{
    incomingQueue.store(other.incomingQueue.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
    outgoingQueue.store(other.outgoingQueue.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
    other.clientId = nullptr;
}

MidiClient& MidiClient::operator=(MidiClient&& other) noexcept
{
    if (this != &other)
    {
        if (clientId)
        {
            if (auto* server = MidiServer::getInstanceWithoutCreating())
                server->unregisterClient(clientId);
        }

        clientId = other.clientId;
        incomingQueue.store(
            other.incomingQueue.exchange(nullptr, std::memory_order_acq_rel),
            std::memory_order_release
        );
        outgoingQueue.store(
            other.outgoingQueue.exchange(nullptr, std::memory_order_acq_rel),
            std::memory_order_release
        );
        other.clientId = nullptr;
    }
    return *this;
}

void MidiClient::getPendingMidi(juce::MidiBuffer& outBuffer, int numSamples, double sampleRate)
{
    juce::ignoreUnused(sampleRate);
    if (auto queue = incomingQueue.load(std::memory_order_acquire))
        queue->popAll(outBuffer, numSamples);
}

void MidiClient::sendMidi(const juce::MidiBuffer& messages)
{
    auto queue = outgoingQueue.load(std::memory_order_acquire);
    if (!queue)
        return;

    for (const auto metadata : messages)
        if (!queue->push(metadata.getMessage(), metadata.samplePosition))
            break; // Queue full
}

void MidiClient::injectMidi(const juce::MidiBuffer& messages)
{
    auto queue = incomingQueue.load(std::memory_order_acquire);
    if (!queue)
        return;

    for (const auto metadata : messages)
        if (!queue->push(metadata.getMessage(), metadata.samplePosition))
            break;
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

    DBG("[MidiServer] Initializing...");

    juce::String error = deviceManager.initialise(0, 0, nullptr, true, {}, nullptr);

    if (error.isNotEmpty())
    {
        DBG("[MidiServer] Failed to initialize AudioDeviceManager: " + error);
        return;
    }

    // MIDI inputs are now opened on-demand based on client subscriptions
    // See updateMidiDeviceSubscriptions()

    startTimer(10);
    initialized = true;
    DBG("[MidiServer] Initialized successfully");
}

void MidiServer::shutdown()
{
    if (!initialized)
        return;

    DBG("[MidiServer] Shutting down...");

    stopTimer();

    {
        juce::ScopedLock lock(clientsMutex);

        for (juce::HashMap<juce::String, juce::MidiOutput*>::Iterator it(outputDevices); it.next();)
            delete it.getValue();
        outputDevices.clear();

        // Disable all enabled MIDI inputs
        for (const auto& [name, identifier] : enabledInputDevices)
        {
            deviceManager.setMidiInputDeviceEnabled(identifier, false);
            deviceManager.removeMidiInputDeviceCallback(identifier, this);
        }
        enabledInputDevices.clear();

        clients.clear();
    }

    deviceManager.closeAudioDevice();
    initialized = false;
    DBG("[MidiServer] Shutdown complete");
}

void MidiServer::registerClient(
    void* clientId,
    const MidiClientState& state,
    int queueSize,
    std::shared_ptr<MidiMessageQueue>& outIncomingQueue,
    std::shared_ptr<MidiMessageQueue>& outOutgoingQueue
)
{
    juce::ignoreUnused(queueSize);

    if (!initialized || clientId == nullptr)
        return;

    juce::ScopedLock lock(clientsMutex);

    ClientInfo info;
    info.state = state;
    info.incomingMidiQueue = outIncomingQueue;
    info.outgoingMidiQueue = outOutgoingQueue;
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

    auto snapshot = activeSnapshot.load(std::memory_order_acquire);
    if (!snapshot)
        return;

    auto it = snapshot->inputSubscriptions.find(sourceName);
    if (it == snapshot->inputSubscriptions.end())
        return;

    for (const auto& clientSnapshot : it->second)
        if (clientSnapshot.incomingMidiQueue)
            clientSnapshot.incomingMidiQueue->push(message, 0);
}

void MidiServer::timerCallback()
{
    if (!initialized)
        return;

    juce::Array<void*> clientList;
    {
        juce::ScopedLock lock(clientsMutex);
        for (auto& [clientId, info] : clients)
            clientList.add(clientId);
    }

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

        juce::MidiBuffer outgoingMessages;
        info->outgoingMidiQueue->popAll(outgoingMessages, INT_MAX);

        if (outgoingMessages.isEmpty())
            continue;

        for (const auto& deviceName : outputDeviceNames)
        {
            juce::MidiOutput* output = nullptr;

            {
                juce::ScopedLock lock(clientsMutex);
                output = outputDevices[deviceName];

                if (output == nullptr)
                {
                    auto devices = juce::MidiOutput::getAvailableDevices();
                    for (const auto& device : devices)
                    {
                        if (device.name == deviceName)
                        {
                            output = juce::MidiOutput::openDevice(device.identifier).release();
                            if (output != nullptr)
                            {
                                outputDevices.set(deviceName, output);
                                DBG("[MidiServer] Opened MIDI output: " + deviceName);
                            }
                            break;
                        }
                    }
                }
            }

            if (output != nullptr)
                for (const auto metadata : outgoingMessages)
                    output->sendMessageNow(metadata.getMessage());
        }
    }
}

void MidiServer::rebuildClientSnapshot()
{
    auto newSnapshot = std::make_shared<DeviceSnapshot>();

    for (auto& [clientId, info] : clients)
    {
        for (const auto& deviceName : info.state.subscribedInputDevices)
        {
            ClientSnapshot snapshot;
            snapshot.incomingMidiQueue = info.incomingMidiQueue;
            snapshot.outgoingMidiQueue = info.outgoingMidiQueue;
            snapshot.state = info.state;
            newSnapshot->inputSubscriptions[deviceName].push_back(std::move(snapshot));
        }

        for (const auto& deviceName : info.state.subscribedOutputDevices)
        {
            ClientSnapshot snapshot;
            snapshot.incomingMidiQueue = info.incomingMidiQueue;
            snapshot.outgoingMidiQueue = info.outgoingMidiQueue;
            snapshot.state = info.state;
            newSnapshot->outputSubscriptions[deviceName].push_back(std::move(snapshot));
        }
    }

    activeSnapshot.store(newSnapshot, std::memory_order_release);
}

void MidiServer::updateMidiDeviceSubscriptions()
{
    // Collect all needed devices from client subscriptions
    juce::StringArray neededInputs, neededOutputs;
    for (const auto& [clientPtr, info] : clients)
    {
        for (const auto& device : info.state.subscribedInputDevices)
            neededInputs.addIfNotAlreadyThere(device);
        for (const auto& device : info.state.subscribedOutputDevices)
            neededOutputs.addIfNotAlreadyThere(device);
    }

    // Build name->identifier map for available inputs
    std::unordered_map<juce::String, juce::String> availableInputMap;
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        availableInputMap[device.name] = device.identifier;

    // Enable newly needed input devices
    for (const auto& name : neededInputs)
    {
        if (enabledInputDevices.count(name) == 0)
        {
            auto it = availableInputMap.find(name);
            if (it != availableInputMap.end())
            {
                deviceManager.setMidiInputDeviceEnabled(it->second, true);
                deviceManager.addMidiInputDeviceCallback(it->second, this);
                enabledInputDevices[name] = it->second;
                DBG("[MidiServer] Enabled MIDI input: " + name);
            }
        }
    }

    // Disable input devices no longer needed
    for (auto it = enabledInputDevices.begin(); it != enabledInputDevices.end();)
        if (!neededInputs.contains(it->first))
        {
            deviceManager.setMidiInputDeviceEnabled(it->second, false);
            deviceManager.removeMidiInputDeviceCallback(it->second, this);
            DBG("[MidiServer] Disabled MIDI input: " + it->first);
            it = enabledInputDevices.erase(it);
        }
        else
            ++it;

    // Close output devices no longer needed (they're opened lazily in timerCallback)
    // Collect keys to remove first to avoid iterator invalidation
    juce::StringArray outputsToRemove;
    for (juce::HashMap<juce::String, juce::MidiOutput*>::Iterator it(outputDevices); it.next();)
        if (!neededOutputs.contains(it.getKey()))
            outputsToRemove.add(it.getKey());
    for (const auto& key : outputsToRemove)
    {
        delete outputDevices[key];
        outputDevices.remove(key);
    }
}

} // namespace atk
