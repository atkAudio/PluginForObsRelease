#pragma once

#include <atkaudio/AtomicSharedPtr.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include <unordered_map>

namespace atk
{

class MidiServer;
class MidiOutboundSenderThread;

struct MidiClientState
{
    juce::StringArray subscribedInputDevices;
    juce::StringArray subscribedOutputDevices;
    juce::StringArray subscribedInputDeviceIdentifiers;
    juce::StringArray subscribedOutputDeviceIdentifiers;

    juce::String serialize() const;
    void deserialize(const juce::String& data);
};

struct MidiInputEvent
{
    juce::MidiMessage message;
    int samplePosition = 0;
    juce::String inputDeviceName;
    juce::String outputDeviceName;
    double arrivalTimeSeconds = 0.0;
};

struct MidiOutputEvent
{
    juce::MidiMessage message;
    juce::String outputDeviceName;
    unsigned long long sequence = 0;
    double scheduledTimeMs = 0.0;
};

class MidiMessageQueue;
class MidiOutputQueue;

class MidiClient
{
public:
    explicit MidiClient(int queueSize = 65536);
    ~MidiClient();

    MidiClient(const MidiClient&) = delete;
    MidiClient& operator=(const MidiClient&) = delete;
    MidiClient(MidiClient&&) noexcept;
    MidiClient& operator=(MidiClient&&) noexcept;

    void getPendingMidi(juce::MidiBuffer& outBuffer, int numSamples, double sampleRate, double blockEndTimeMs = 0.0);
    void getPendingMidiEvents(std::vector<MidiInputEvent>& outEvents, int numSamples, double sampleRate);

    void sendMidi(const juce::MidiBuffer& messages, double blockStartTimeMs = 0.0, double sampleRate = 0.0);
    void sendMidiToDevice(const juce::MidiBuffer& messages, juce::String outputDeviceName);

    void injectMidi(const juce::MidiBuffer& messages);

    void setSubscriptions(const MidiClientState& state);

    MidiClientState getSubscriptions() const;

    void* getClientId() const
    {
        return clientId;
    }

private:
    void* clientId;
    AtomicSharedPtr<MidiMessageQueue> incomingQueue;
    AtomicSharedPtr<MidiOutputQueue> outboundQueue;
};

class MidiMessageQueue
{
public:
    static constexpr int kDefaultQueueSize = 65536;

    explicit MidiMessageQueue(int queueSize = kDefaultQueueSize)
        : fifo(queueSize)
    {
        messages.resize(queueSize);
    }

    bool push(
        const juce::MidiMessage& message,
        int samplePosition,
        const juce::String& inputDeviceName = {},
        const juce::String& outputDeviceName = {},
        double arrivalTimeSeconds = 0.0
    )
    {
        juce::SpinLock::ScopedLockType lock(producerLock);

        int start1, size1, start2, size2;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            messages[start1].message = message;
            messages[start1].samplePosition =
                (samplePosition == 0) ? autoIncrementPosition.fetch_add(1, std::memory_order_relaxed) : samplePosition;
            messages[start1].inputDeviceName = inputDeviceName;
            messages[start1].outputDeviceName = outputDeviceName;
            messages[start1].arrivalTimeSeconds = arrivalTimeSeconds;
            fifo.finishedWrite(1);
            return true;
        }
        return false;
    }

    void
    popAll(juce::MidiBuffer& outBuffer, int maxSamples = 65536, double sampleRate = 0.0, double blockEndTimeMs = 0.0)
    {
        int start1, size1, start2, size2;
        const int numReady = fifo.getNumReady();

        if (numReady > 0)
        {
            fifo.prepareToRead(numReady, start1, size1, start2, size2);
            const int maxPos = (maxSamples > 0) ? maxSamples - 1 : 0;
            auto samplePosition = [&](const MidiInputEvent& event)
            {
                if (sampleRate > 0.0 && blockEndTimeMs > 0.0 && event.arrivalTimeSeconds > 0.0)
                {
                    double position =
                        (event.arrivalTimeSeconds * 1000.0 - blockEndTimeMs) * sampleRate / 1000.0 + maxSamples;
                    return static_cast<int>(std::clamp(position, 0.0, static_cast<double>(maxPos)));
                }
                return std::clamp(event.samplePosition, 0, maxPos);
            };

            for (int i = 0; i < size1; ++i)
            {
                const auto& msg = messages[start1 + i];
                outBuffer.addEvent(msg.message, samplePosition(msg));
            }

            for (int i = 0; i < size2; ++i)
            {
                const auto& msg = messages[start2 + i];
                outBuffer.addEvent(msg.message, samplePosition(msg));
            }

            fifo.finishedRead(size1 + size2);
            autoIncrementPosition.store(0, std::memory_order_relaxed);
        }
    }

    void popAllDetailed(std::vector<MidiInputEvent>& outEvents, int maxSamples = 65536)
    {
        int start1, size1, start2, size2;
        const int numReady = fifo.getNumReady();

        if (numReady <= 0)
            return;

        fifo.prepareToRead(numReady, start1, size1, start2, size2);
        const int maxPos = (maxSamples > 0) ? maxSamples - 1 : 0;

        outEvents.reserve(outEvents.size() + size1 + size2);

        for (int i = 0; i < size1; ++i)
        {
            const auto& msg = messages[start1 + i];
            outEvents.push_back(msg);
            outEvents.back().samplePosition = std::clamp(msg.samplePosition, 0, maxPos);
        }

        for (int i = 0; i < size2; ++i)
        {
            const auto& msg = messages[start2 + i];
            outEvents.push_back(msg);
            outEvents.back().samplePosition = std::clamp(msg.samplePosition, 0, maxPos);
        }

        fifo.finishedRead(size1 + size2);
        autoIncrementPosition.store(0, std::memory_order_relaxed);
    }

    void clear()
    {
        int start1, size1, start2, size2;
        const int numReady = fifo.getNumReady();
        if (numReady > 0)
        {
            fifo.prepareToRead(numReady, start1, size1, start2, size2);
            fifo.finishedRead(size1 + size2);
        }
    }

    int getNumReady() const
    {
        return fifo.getNumReady();
    }

private:
    juce::SpinLock producerLock;
    std::atomic<int> autoIncrementPosition{0};
    juce::AbstractFifo fifo;
    std::vector<MidiInputEvent> messages;
};

class MidiOutputQueue
{
public:
    static constexpr int kDefaultQueueSize = 2048;

    explicit MidiOutputQueue(int queueSize = kDefaultQueueSize)
        : fifo(queueSize)
    {
        messages.resize(queueSize);
    }

    bool push(const juce::MidiMessage& message, const juce::String& outputDeviceName = {}, double scheduledTimeMs = 0.0)
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 <= 0)
        {
            droppedMessages.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        messages[start1].message = message;
        messages[start1].outputDeviceName = outputDeviceName;
        messages[start1].sequence = nextSequence.fetch_add(1, std::memory_order_relaxed);
        messages[start1].scheduledTimeMs = scheduledTimeMs;
        fifo.finishedWrite(1);
        return true;
    }

    void popAll(std::vector<MidiOutputEvent>& outEvents)
    {
        int start1, size1, start2, size2;
        int numReady = fifo.getNumReady();

        if (numReady <= 0)
            return;

        fifo.prepareToRead(numReady, start1, size1, start2, size2);
        outEvents.reserve(outEvents.size() + size1 + size2);

        for (int i = 0; i < size1; ++i)
            outEvents.push_back(messages[start1 + i]);

        for (int i = 0; i < size2; ++i)
            outEvents.push_back(messages[start2 + i]);

        fifo.finishedRead(size1 + size2);
    }

    void clear()
    {
        int start1, size1, start2, size2;
        int numReady = fifo.getNumReady();
        if (numReady <= 0)
            return;

        fifo.prepareToRead(numReady, start1, size1, start2, size2);
        fifo.finishedRead(size1 + size2);
    }

    unsigned long long getDroppedMessagesCount() const
    {
        return droppedMessages.load(std::memory_order_relaxed);
    }

private:
    std::atomic<unsigned long long> nextSequence{0};
    std::atomic<unsigned long long> droppedMessages{0};
    juce::AbstractFifo fifo;
    std::vector<MidiOutputEvent> messages;
};

class MidiServer
    : public juce::DeletedAtShutdown
    , private juce::MidiInputCallback
    , private juce::Timer
{
public:
    friend class MidiOutboundSenderThread;

    JUCE_DECLARE_SINGLETON(MidiServer, false)
    ~MidiServer() override;

    void initialize();
    void shutdown();

    void registerClient(
        void* clientId,
        const MidiClientState& state,
        int queueSize,
        std::shared_ptr<MidiMessageQueue>& outIncomingQueue,
        std::shared_ptr<MidiOutputQueue>& outOutboundQueue
    );
    void unregisterClient(void* clientId);
    void updateClientSubscriptions(void* clientId, const MidiClientState& state);
    MidiClientState getClientState(void* clientId) const;

    juce::StringArray getAvailableMidiInputDevices() const;
    juce::StringArray getAvailableMidiOutputDevices() const;
    void getPendingMonitorMidiEvents(std::vector<MidiInputEvent>& outEvents, int numSamples = 65536);
    void signalOutboundMidiReady();

private:
    MidiServer();

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void timerCallback() override;
    void drainOutboundMidi();
    void dispatchOutboundMidi(
        const juce::MidiMessage& message,
        const MidiClientState& state,
        const juce::String& outputDeviceName,
        double scheduledTimeMs
    );
    void
    dispatchMidiToOutput(const juce::MidiMessage& message, const juce::String& requestedKey, double scheduledTimeMs);
    void syncMidiInputDevices();
    void syncMidiOutputDevices();
    void syncMidiDevices();
    void rebuildClientSnapshot();

    struct ClientInfo
    {
        MidiClientState state;
        std::shared_ptr<MidiMessageQueue> incomingMidiQueue;
        std::shared_ptr<MidiOutputQueue> outboundMidiQueue;
    };

    struct ClientSnapshot
    {
        std::shared_ptr<MidiMessageQueue> incomingMidiQueue;
        MidiClientState state;
    };

    struct DeviceSnapshot
    {
        std::unordered_map<juce::String, std::vector<ClientSnapshot>> inputSubscriptions;
        std::unordered_map<juce::String, std::vector<ClientSnapshot>> outputSubscriptions;
    };

    std::unordered_map<juce::String, std::unique_ptr<juce::MidiInput>> inputDevices;
    mutable juce::CriticalSection clientsMutex;
    std::unordered_map<void*, ClientInfo> clients;
    AtomicSharedPtr<DeviceSnapshot> activeSnapshot{std::make_shared<DeviceSnapshot>()};
    std::shared_ptr<MidiMessageQueue> monitorQueue;
    std::unique_ptr<MidiOutboundSenderThread> outboundSenderThread;
    juce::HashMap<juce::String, juce::MidiOutput*> outputDevices;
    juce::HashMap<juce::String, juce::String> activeInputDevices; // identifier -> name
    bool initialized = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiServer)
};

} // namespace atk
