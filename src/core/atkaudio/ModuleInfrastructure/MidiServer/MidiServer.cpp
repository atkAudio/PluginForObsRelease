#include "MidiServer.h"

#include <atkaudio/Logging.h>

namespace atk
{

JUCE_IMPLEMENT_SINGLETON(MidiServer)

namespace
{
struct MidiDeviceLookupResult
{
    juce::String cacheKey;
    juce::String deviceName;
    bool found = false;
};

MidiDeviceLookupResult
resolveMidiDeviceLookup(const juce::Array<juce::MidiDeviceInfo>& devices, const juce::String& requestedKey)
{
    MidiDeviceLookupResult result;

    if (requestedKey.isEmpty())
        return result;

    for (int i = 0; i < devices.size(); ++i)
    {
        const juce::MidiDeviceInfo& device = devices[i];

        if (device.identifier != requestedKey && device.name != requestedKey)
            continue;

        result.cacheKey = device.identifier.isNotEmpty() ? device.identifier : device.name;
        result.deviceName = device.name;
        result.found = true;
        return result;
    }

    return result;
}

bool areClientStatesEqual(const MidiClientState& left, const MidiClientState& right)
{
    if (left.subscribedInputDevices.size() != right.subscribedInputDevices.size())
        return false;

    if (left.subscribedOutputDevices.size() != right.subscribedOutputDevices.size())
        return false;

    if (left.subscribedInputDeviceIdentifiers.size() != right.subscribedInputDeviceIdentifiers.size())
        return false;

    if (left.subscribedOutputDeviceIdentifiers.size() != right.subscribedOutputDeviceIdentifiers.size())
        return false;

    for (int i = 0; i < left.subscribedInputDevices.size(); ++i)
        if (left.subscribedInputDevices[i] != right.subscribedInputDevices[i])
            return false;

    for (int i = 0; i < left.subscribedOutputDevices.size(); ++i)
        if (left.subscribedOutputDevices[i] != right.subscribedOutputDevices[i])
            return false;

    for (int i = 0; i < left.subscribedInputDeviceIdentifiers.size(); ++i)
        if (left.subscribedInputDeviceIdentifiers[i] != right.subscribedInputDeviceIdentifiers[i])
            return false;

    for (int i = 0; i < left.subscribedOutputDeviceIdentifiers.size(); ++i)
        if (left.subscribedOutputDeviceIdentifiers[i] != right.subscribedOutputDeviceIdentifiers[i])
            return false;

    return true;
}

juce::String resolveIdentifierForName(
    const juce::Array<juce::MidiDeviceInfo>& devices,
    const juce::String& deviceName,
    const juce::String& preferredIdentifier
)
{
    if (preferredIdentifier.isNotEmpty())
        for (auto& device : devices)
            if (device.identifier == preferredIdentifier)
                return preferredIdentifier;

    if (deviceName.isEmpty())
        return {};

    for (auto& device : devices)
        if (device.name == deviceName)
            return device.identifier;

    return {};
}

MidiClientState normalizeStateWithDeviceIdentifiers(const MidiClientState& rawState)
{
    MidiClientState normalized = rawState;

    auto availableInputDevices = juce::MidiInput::getAvailableDevices();
    auto availableOutputDevices = juce::MidiOutput::getAvailableDevices();

    normalized.subscribedInputDeviceIdentifiers.clear();
    normalized.subscribedOutputDeviceIdentifiers.clear();

    for (int i = 0; i < normalized.subscribedInputDevices.size(); ++i)
    {
        juce::String preferredIdentifier;
        if (i < rawState.subscribedInputDeviceIdentifiers.size())
            preferredIdentifier = rawState.subscribedInputDeviceIdentifiers[i];

        normalized.subscribedInputDeviceIdentifiers.add(
            resolveIdentifierForName(availableInputDevices, normalized.subscribedInputDevices[i], preferredIdentifier)
        );
    }

    for (int i = 0; i < normalized.subscribedOutputDevices.size(); ++i)
    {
        juce::String preferredIdentifier;
        if (i < rawState.subscribedOutputDeviceIdentifiers.size())
            preferredIdentifier = rawState.subscribedOutputDeviceIdentifiers[i];

        normalized.subscribedOutputDeviceIdentifiers.add(
            resolveIdentifierForName(availableOutputDevices, normalized.subscribedOutputDevices[i], preferredIdentifier)
        );
    }

    return normalized;
}
} // namespace

class MidiOutboundSenderThread : public juce::Thread
{
public:
    explicit MidiOutboundSenderThread(MidiServer& ownerIn)
        : juce::Thread("MidiOutboundSender")
        , owner(ownerIn)
    {
    }

    void run() override
    {
        while (!threadShouldExit())
        {
            owner.drainOutboundMidi();
            wait(1.0);
        }

        owner.drainOutboundMidi();
    }

private:
    MidiServer& owner;
};

juce::String MidiClientState::serialize() const
{
    juce::XmlElement xml("MidiClientState");

    for (int i = 0; i < subscribedInputDevices.size(); ++i)
    {
        auto* inputElement = xml.createNewChildElement("InputDevice");
        inputElement->setAttribute("name", subscribedInputDevices[i]);

        if (i < subscribedInputDeviceIdentifiers.size() && subscribedInputDeviceIdentifiers[i].isNotEmpty())
            inputElement->setAttribute("identifier", subscribedInputDeviceIdentifiers[i]);
    }

    for (int i = 0; i < subscribedOutputDevices.size(); ++i)
    {
        auto* outputElement = xml.createNewChildElement("OutputDevice");
        outputElement->setAttribute("name", subscribedOutputDevices[i]);

        if (i < subscribedOutputDeviceIdentifiers.size() && subscribedOutputDeviceIdentifiers[i].isNotEmpty())
            outputElement->setAttribute("identifier", subscribedOutputDeviceIdentifiers[i]);
    }

    return xml.toString();
}

void MidiClientState::deserialize(const juce::String& data)
{
    subscribedInputDevices.clear();
    subscribedOutputDevices.clear();
    subscribedInputDeviceIdentifiers.clear();
    subscribedOutputDeviceIdentifiers.clear();

    if (auto xml = juce::parseXML(data))
    {
        for (auto* child : xml->getChildIterator())
            if (child->hasTagName("InputDevice"))
            {
                subscribedInputDevices.add(child->getStringAttribute("name"));
                subscribedInputDeviceIdentifiers.add(child->getStringAttribute("identifier"));
            }
            else if (child->hasTagName("OutputDevice"))
            {
                subscribedOutputDevices.add(child->getStringAttribute("name"));
                subscribedOutputDeviceIdentifiers.add(child->getStringAttribute("identifier"));
            }
    }
}

MidiClient::MidiClient(int queueSize)
    : clientId(this)
    , incomingQueue(std::make_shared<MidiMessageQueue>(queueSize))
    , outboundQueue(std::make_shared<MidiOutputQueue>(queueSize))
{
    if (auto* server = MidiServer::getInstance())
    {
        auto incoming = incomingQueue.load(std::memory_order_acquire);
        auto outbound = outboundQueue.load(std::memory_order_acquire);
        server->registerClient(clientId, MidiClientState(), queueSize, incoming, outbound);
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
    outboundQueue.store(other.outboundQueue.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
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
        incomingQueue.store(other.incomingQueue.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
        outboundQueue.store(other.outboundQueue.exchange(nullptr, std::memory_order_acq_rel), std::memory_order_release);
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

void MidiClient::getPendingMidiEvents(std::vector<MidiInputEvent>& outEvents, int numSamples, double sampleRate)
{
    juce::ignoreUnused(sampleRate);
    if (auto queue = incomingQueue.load(std::memory_order_acquire))
        queue->popAllDetailed(outEvents, numSamples);
}

void MidiClient::sendMidi(const juce::MidiBuffer& messages)
{
    auto queue = outboundQueue.load(std::memory_order_acquire);
    if (!queue)
        return;

    bool queuedAnyMessages = false;
    for (const auto metadata : messages)
    {
        if (!queue->push(metadata.getMessage()))
            break; // Queue full

        queuedAnyMessages = true;
    }

    if (queuedAnyMessages)
        if (auto* server = MidiServer::getInstanceWithoutCreating())
            server->signalOutboundMidiReady();
}

void MidiClient::sendMidiToDevice(const juce::MidiBuffer& messages, juce::String outputDeviceName)
{
    auto queue = outboundQueue.load(std::memory_order_acquire);
    if (!queue || outputDeviceName.isEmpty())
        return;

    bool queuedAnyMessages = false;
    for (const auto metadata : messages)
    {
        if (!queue->push(metadata.getMessage(), outputDeviceName))
            break;

        queuedAnyMessages = true;
    }

    if (queuedAnyMessages)
        if (auto* server = MidiServer::getInstanceWithoutCreating())
            server->signalOutboundMidiReady();
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
    : monitorQueue(std::make_shared<MidiMessageQueue>())
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

    atk::logging::info("MidiServer::initialize", "begin");
    juce::String error = deviceManager.initialise(0, 0, nullptr, true, {}, nullptr);

    if (error.isNotEmpty())
    {
        atk::logging::warning(
            "MidiServer::initialize",
            juce::String::formatted("AudioDeviceManager initialise failed: %s", error.toRawUTF8())
        );
        return;
    }

    initialized = true;

    outboundSenderThread = std::make_unique<MidiOutboundSenderThread>(*this);
    if (!outboundSenderThread->startThread(juce::Thread::Priority::high))
    {
        outboundSenderThread.reset();
        initialized = false;
        atk::logging::warning("MidiServer::initialize", "failed to start outbound sender thread");
        return;
    }

    startTimer(500);
    atk::logging::info("MidiServer::initialize", "completed");
}

void MidiServer::shutdown()
{
    if (!initialized)
        return;

    atk::logging::info("MidiServer::shutdown", "begin");
    stopTimer();

    if (outboundSenderThread)
    {
        outboundSenderThread->signalThreadShouldExit();
        outboundSenderThread->notify();
        outboundSenderThread->stopThread(1000);
        outboundSenderThread.reset();
    }

    {
        juce::ScopedLock lock(clientsMutex);

        for (juce::HashMap<juce::String, juce::MidiOutput*>::Iterator it(outputDevices); it.next();)
            delete it.getValue();
        outputDevices.clear();

        // Disable all active MIDI inputs
        juce::StringArray inputDeviceIdentifiersToRemove;
        for (juce::HashMap<juce::String, juce::String>::Iterator it(activeInputDevices); it.next();)
            inputDeviceIdentifiersToRemove.add(it.getKey());

        for (const auto& identifier : inputDeviceIdentifiersToRemove)
        {
            deviceManager.setMidiInputDeviceEnabled(identifier, false);
            deviceManager.removeMidiInputDeviceCallback(identifier, this);
        }
        activeInputDevices.clear();

        clients.clear();
    }

    deviceManager.closeAudioDevice();
    initialized = false;
    atk::logging::info("MidiServer::shutdown", "completed");
}

void MidiServer::registerClient(
    void* clientId,
    const MidiClientState& state,
    int queueSize,
    std::shared_ptr<MidiMessageQueue>& outIncomingQueue,
    std::shared_ptr<MidiOutputQueue>& outOutboundQueue
)
{
    juce::ignoreUnused(queueSize);

    if (!initialized || clientId == nullptr)
        return;

    {
        juce::ScopedLock lock(clientsMutex);

        ClientInfo info;
        info.state = normalizeStateWithDeviceIdentifiers(state);
        info.incomingMidiQueue = outIncomingQueue;
        info.outboundMidiQueue = outOutboundQueue;
        clients.insert_or_assign(clientId, std::move(info));

        rebuildClientSnapshot();
    }

    syncMidiDevices();
}

void MidiServer::unregisterClient(void* clientId)
{
    if (!initialized || clientId == nullptr)
        return;

    {
        juce::ScopedLock lock(clientsMutex);
        clients.erase(clientId);
        rebuildClientSnapshot();
    }

    syncMidiDevices();
}

void MidiServer::updateClientSubscriptions(void* clientId, const MidiClientState& state)
{
    if (!initialized || clientId == nullptr)
        return;

    bool stateChanged = false;

    {
        juce::ScopedLock lock(clientsMutex);
        auto normalizedState = normalizeStateWithDeviceIdentifiers(state);

        if (clients.contains(clientId))
        {
            if (areClientStatesEqual(clients.at(clientId).state, normalizedState))
                return;

            clients.at(clientId).state = normalizedState;
            rebuildClientSnapshot();
            stateChanged = true;
        }
    }

    if (stateChanged)
        syncMidiDevices();
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

void MidiServer::getPendingMonitorMidiEvents(std::vector<MidiInputEvent>& outEvents, int numSamples)
{
    if (auto queue = monitorQueue)
        queue->popAllDetailed(outEvents, numSamples);
}

void MidiServer::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    if (!initialized || source == nullptr)
        return;

    juce::String sourceName = source->getName();
    juce::String sourceIdentifier;

    for (juce::HashMap<juce::String, juce::String>::Iterator it(activeInputDevices); it.next();)
    {
        if (it.getValue() == sourceName)
        {
            sourceIdentifier = it.getKey();
            break;
        }
    }

    if (auto queue = monitorQueue)
        queue->push(message, 0, sourceName);

    auto snapshot = activeSnapshot.load(std::memory_order_acquire);
    if (!snapshot)
        return;

    auto dispatchToSubscribers = [&](const juce::String& key) -> bool
    {
        auto it = snapshot->inputSubscriptions.find(key);
        if (it == snapshot->inputSubscriptions.end())
            return false;

        for (const auto& clientSnapshot : it->second)
            if (clientSnapshot.incomingMidiQueue)
                clientSnapshot.incomingMidiQueue->push(message, 0, sourceName);

        return true;
    };

    if (sourceIdentifier.isNotEmpty() && dispatchToSubscribers(sourceIdentifier))
        return;

    dispatchToSubscribers(sourceName);
}

void MidiServer::timerCallback()
{
    if (!initialized)
        return;

    syncMidiDevices();
}

void MidiServer::syncMidiDevices()
{
    syncMidiInputDevices();
    syncMidiOutputDevices();
}

void MidiServer::drainOutboundMidi()
{
    std::vector<std::pair<std::shared_ptr<MidiOutputQueue>, MidiClientState>> clientsToDrain;

    {
        juce::ScopedLock lock(clientsMutex);
        clientsToDrain.reserve(clients.size());

        for (auto& [clientId, info] : clients)
        {
            juce::ignoreUnused(clientId);

            if (!info.outboundMidiQueue)
                continue;

            clientsToDrain.push_back({info.outboundMidiQueue, info.state});
        }
    }

    for (auto& client : clientsToDrain)
    {
        auto queue = client.first;
        auto& state = client.second;

        if (!queue)
            continue;

        std::vector<MidiOutputEvent> outboundEvents;
        queue->popAll(outboundEvents);

        for (auto& event : outboundEvents)
            dispatchOutboundMidi(event.message, state, event.outputDeviceName);
    }
}

void MidiServer::signalOutboundMidiReady()
{
    if (outboundSenderThread)
        outboundSenderThread->notify();
}

void MidiServer::dispatchOutboundMidi(
    const juce::MidiMessage& message,
    const MidiClientState& state,
    const juce::String& outputDeviceName
)
{
    if (outputDeviceName.isNotEmpty())
    {
        dispatchMidiToOutput(message, outputDeviceName);
        return;
    }

    for (int i = 0; i < state.subscribedOutputDevices.size(); ++i)
    {
        juce::String preferredIdentifier;
        if (i < state.subscribedOutputDeviceIdentifiers.size())
            preferredIdentifier = state.subscribedOutputDeviceIdentifiers[i];

        juce::String requestedKey =
            preferredIdentifier.isNotEmpty() ? preferredIdentifier : state.subscribedOutputDevices[i];
        dispatchMidiToOutput(message, requestedKey);
    }
}

void MidiServer::dispatchMidiToOutput(const juce::MidiMessage& message, const juce::String& requestedKey)
{
    if (requestedKey.isEmpty())
        return;

    {
        juce::ScopedLock lock(clientsMutex);
        if (auto* output = outputDevices[requestedKey])
        {
            output->sendMessageNow(message);
            return;
        }
    }

    auto availableOutputs = juce::MidiOutput::getAvailableDevices();
    auto lookup = resolveMidiDeviceLookup(availableOutputs, requestedKey);
    if (!lookup.found)
        return;

    {
        juce::ScopedLock lock(clientsMutex);
        if (auto* output = outputDevices[lookup.cacheKey])
        {
            output->sendMessageNow(message);
            return;
        }
    }

    for (int i = 0; i < availableOutputs.size(); ++i)
    {
        auto& device = availableOutputs.getReference(i);
        if (device.identifier != lookup.cacheKey && device.name != lookup.cacheKey)
            continue;

        juce::MidiOutput* openedOutput = juce::MidiOutput::openDevice(device.identifier).release();
        if (openedOutput == nullptr)
            return;

        {
            juce::ScopedLock lock(clientsMutex);
            outputDevices.set(lookup.cacheKey, openedOutput);
            openedOutput->sendMessageNow(message);
        }

        atk::logging::debug(
            "MidiServer::dispatchMidiToOutput",
            juce::String::formatted("opened MIDI output \"%s\"", device.name.toRawUTF8())
        );
        return;
    }
}

void MidiServer::rebuildClientSnapshot()
{
    auto newSnapshot = std::make_shared<DeviceSnapshot>();

    for (auto& [clientId, info] : clients)
    {
        for (int i = 0; i < info.state.subscribedInputDevices.size(); ++i)
        {
            auto deviceName = info.state.subscribedInputDevices[i];
            juce::String deviceIdentifier;
            if (i < info.state.subscribedInputDeviceIdentifiers.size())
                deviceIdentifier = info.state.subscribedInputDeviceIdentifiers[i];

            ClientSnapshot snapshot;
            snapshot.incomingMidiQueue = info.incomingMidiQueue;
            snapshot.state = info.state;

            if (deviceIdentifier.isNotEmpty())
                newSnapshot->inputSubscriptions[deviceIdentifier].push_back(snapshot);

            if (deviceName.isNotEmpty() && deviceName != deviceIdentifier)
                newSnapshot->inputSubscriptions[deviceName].push_back(std::move(snapshot));
        }

        for (int i = 0; i < info.state.subscribedOutputDevices.size(); ++i)
        {
            auto deviceName = info.state.subscribedOutputDevices[i];
            juce::String deviceIdentifier;
            if (i < info.state.subscribedOutputDeviceIdentifiers.size())
                deviceIdentifier = info.state.subscribedOutputDeviceIdentifiers[i];

            ClientSnapshot snapshot;
            snapshot.incomingMidiQueue = info.incomingMidiQueue;
            snapshot.state = info.state;

            if (deviceIdentifier.isNotEmpty())
                newSnapshot->outputSubscriptions[deviceIdentifier].push_back(snapshot);

            if (deviceName.isNotEmpty() && deviceName != deviceIdentifier)
                newSnapshot->outputSubscriptions[deviceName].push_back(std::move(snapshot));
        }
    }

    activeSnapshot.store(newSnapshot, std::memory_order_release);
}

void MidiServer::syncMidiInputDevices()
{
    juce::Array<juce::MidiDeviceInfo> availableInputs = juce::MidiInput::getAvailableDevices();
    juce::HashMap<juce::String, juce::String> desiredInputDevices;

    {
        auto snapshot = activeSnapshot.load(std::memory_order_acquire);

        if (snapshot != nullptr)
        {
            for (auto it = snapshot->inputSubscriptions.begin(); it != snapshot->inputSubscriptions.end(); ++it)
            {
                MidiDeviceLookupResult lookup = resolveMidiDeviceLookup(availableInputs, it->first);
                if (lookup.found)
                    desiredInputDevices.set(lookup.cacheKey, lookup.deviceName);
            }
        }
    }

    for (juce::HashMap<juce::String, juce::String>::Iterator it(desiredInputDevices); it.next();)
    {
        if (activeInputDevices.contains(it.getKey()))
            continue;

        deviceManager.setMidiInputDeviceEnabled(it.getKey(), true);
        deviceManager.addMidiInputDeviceCallback(it.getKey(), this);
        activeInputDevices.set(it.getKey(), it.getValue());
        atk::logging::debug(
            "MidiServer::syncMidiInputDevices",
            juce::String::formatted("enabled MIDI input \"%s\"", it.getValue().toRawUTF8())
        );
    }

    juce::StringArray inputDeviceIdentifiersToRemove;
    for (juce::HashMap<juce::String, juce::String>::Iterator it(activeInputDevices); it.next();)
        if (!desiredInputDevices.contains(it.getKey()))
            inputDeviceIdentifiersToRemove.add(it.getKey());

    for (int i = 0; i < inputDeviceIdentifiersToRemove.size(); ++i)
    {
        juce::String identifier = inputDeviceIdentifiersToRemove[i];
        juce::String name = activeInputDevices[identifier];
        deviceManager.setMidiInputDeviceEnabled(identifier, false);
        deviceManager.removeMidiInputDeviceCallback(identifier, this);
        atk::logging::debug(
            "MidiServer::syncMidiInputDevices",
            juce::String::formatted("disabled MIDI input \"%s\"", name.toRawUTF8())
        );
        activeInputDevices.remove(identifier);
    }
}

void MidiServer::syncMidiOutputDevices()
{
    juce::Array<juce::MidiDeviceInfo> availableOutputs = juce::MidiOutput::getAvailableDevices();
    juce::HashMap<juce::String, juce::String> desiredOutputDevices;

    {
        auto snapshot = activeSnapshot.load(std::memory_order_acquire);

        if (snapshot != nullptr)
        {
            for (auto it = snapshot->outputSubscriptions.begin(); it != snapshot->outputSubscriptions.end(); ++it)
            {
                MidiDeviceLookupResult lookup = resolveMidiDeviceLookup(availableOutputs, it->first);
                if (lookup.found)
                    desiredOutputDevices.set(lookup.cacheKey, lookup.deviceName);
            }
        }
    }

    juce::StringArray outputDeviceKeysToRemove;
    {
        juce::ScopedLock lock(clientsMutex);

        for (juce::HashMap<juce::String, juce::MidiOutput*>::Iterator it(outputDevices); it.next();)
            if (!desiredOutputDevices.contains(it.getKey()))
                outputDeviceKeysToRemove.add(it.getKey());

        for (int i = 0; i < outputDeviceKeysToRemove.size(); ++i)
        {
            juce::String key = outputDeviceKeysToRemove[i];
            juce::MidiOutput* staleOutput = outputDevices[key];
            if (staleOutput != nullptr)
                delete staleOutput;

            outputDevices.remove(key);
        }
    }

    for (int i = 0; i < outputDeviceKeysToRemove.size(); ++i)
    {
        juce::String key = outputDeviceKeysToRemove[i];
        atk::logging::debug(
            "MidiServer::syncMidiOutputDevices",
            juce::String::formatted("removed stale MIDI output endpoint \"%s\"", key.toRawUTF8())
        );
    }

    for (juce::HashMap<juce::String, juce::String>::Iterator it(desiredOutputDevices); it.next();)
    {
        juce::MidiOutput* output = nullptr;

        {
            juce::ScopedLock lock(clientsMutex);
            output = outputDevices[it.getKey()];
        }

        if (output != nullptr)
            continue;

        for (int i = 0; i < availableOutputs.size(); ++i)
        {
            const juce::MidiDeviceInfo& device = availableOutputs[i];

            if (device.identifier != it.getKey() && device.name != it.getKey())
                continue;

            juce::MidiOutput* openedOutput = juce::MidiOutput::openDevice(device.identifier).release();
            if (openedOutput != nullptr)
            {
                juce::ScopedLock lock(clientsMutex);
                outputDevices.set(it.getKey(), openedOutput);
                atk::logging::debug(
                    "MidiServer::syncMidiOutputDevices",
                    juce::String::formatted("opened MIDI output \"%s\"", device.name.toRawUTF8())
                );
            }

            break;
        }
    }
}

} // namespace atk
