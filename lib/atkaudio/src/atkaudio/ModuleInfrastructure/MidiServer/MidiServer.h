#pragma once

#include <atkaudio/AtomicSharedPtr.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include <unordered_map>

namespace atk
{

class MidiServer;

/**
 * Client subscription state - stores which devices the client is subscribed to
 */
struct MidiClientState
{
    juce::StringArray subscribedInputDevices;
    juce::StringArray subscribedOutputDevices;

    juce::String serialize() const;
    void deserialize(const juce::String& data);
};

class MidiMessageQueue;

/**
 * MIDI Client - owns lock-free queues for real-time safe MIDI I/O
 *
 * REAL-TIME SAFETY:
 * - getPendingMidi(), sendMidi(), injectMidi() are lock-free
 * - setSubscriptions() and getSubscriptions() are NOT real-time safe
 *
 * Usage:
 *   class MyAudioProcessor {
 *       atk::MidiClient midiClient;
 *
 *       void processBlock(AudioBuffer& buffer, MidiBuffer& midi) {
 *           midiClient.getPendingMidi(midi, buffer.getNumSamples(), getSampleRate());
 *           // ... process ...
 *           midiClient.sendMidi(midi);
 *       }
 *   };
 */
class MidiClient
{
public:
    explicit MidiClient(int queueSize = 65536);
    ~MidiClient();

    MidiClient(const MidiClient&) = delete;
    MidiClient& operator=(const MidiClient&) = delete;
    MidiClient(MidiClient&&) noexcept;
    MidiClient& operator=(MidiClient&&) noexcept;

    /** Get pending MIDI from subscribed input devices (real-time safe) */
    void getPendingMidi(juce::MidiBuffer& outBuffer, int numSamples, double sampleRate);

    /** Send MIDI to subscribed output devices (real-time safe) */
    void sendMidi(const juce::MidiBuffer& messages);

    /** Inject MIDI directly - for virtual keyboard etc (real-time safe) */
    void injectMidi(const juce::MidiBuffer& messages);

    /** Update device subscriptions (NOT real-time safe) */
    void setSubscriptions(const MidiClientState& state);

    /** Get current subscriptions (NOT real-time safe) */
    MidiClientState getSubscriptions() const;

    void* getClientId() const
    {
        return clientId;
    }

private:
    void* clientId;
    std::shared_ptr<MidiMessageQueue> incomingQueue;
    std::shared_ptr<MidiMessageQueue> outgoingQueue;
};

/**
 * Thread-safe MIDI message queue (MPSC - Multi-Producer Single-Consumer)
 */
class MidiMessageQueue
{
public:
    static constexpr int kDefaultQueueSize = 65536;

    struct TimestampedMidiMessage
    {
        juce::MidiMessage message;
        int samplePosition = 0;

        TimestampedMidiMessage() = default;

        TimestampedMidiMessage(const juce::MidiMessage& msg, int pos)
            : message(msg)
            , samplePosition(pos)
        {
        }
    };

    explicit MidiMessageQueue(int queueSize = kDefaultQueueSize)
        : fifo(queueSize)
    {
        messages.resize(queueSize);
    }

    /** Push a message (thread-safe for multiple producers) */
    bool push(const juce::MidiMessage& message, int samplePosition)
    {
        juce::SpinLock::ScopedLockType lock(producerLock);

        int start1, size1, start2, size2;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            messages[start1].message = message;
            messages[start1].samplePosition =
                (samplePosition == 0) ? autoIncrementPosition.fetch_add(1, std::memory_order_relaxed) : samplePosition;
            fifo.finishedWrite(1);
            return true;
        }
        return false;
    }

    /** Pop all messages into buffer (real-time safe, single consumer) */
    void popAll(juce::MidiBuffer& outBuffer, int maxSamples = 65536)
    {
        int start1, size1, start2, size2;
        const int numReady = fifo.getNumReady();

        if (numReady > 0)
        {
            fifo.prepareToRead(numReady, start1, size1, start2, size2);
            const int maxPos = (maxSamples > 0) ? maxSamples - 1 : 0;

            for (int i = 0; i < size1; ++i)
            {
                const auto& msg = messages[start1 + i];
                outBuffer.addEvent(msg.message, std::clamp(msg.samplePosition, 0, maxPos));
            }

            for (int i = 0; i < size2; ++i)
            {
                const auto& msg = messages[start2 + i];
                outBuffer.addEvent(msg.message, std::clamp(msg.samplePosition, 0, maxPos));
            }

            fifo.finishedRead(size1 + size2);
            autoIncrementPosition.store(0, std::memory_order_relaxed);
        }
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
    std::vector<TimestampedMidiMessage> messages;
};

/**
 * Global MIDI Server singleton - routes MIDI between devices and clients
 */
class MidiServer
    : public juce::DeletedAtShutdown
    , private juce::MidiInputCallback
    , private juce::Timer
{
public:
    JUCE_DECLARE_SINGLETON(MidiServer, false)
    ~MidiServer() override;

    void initialize();
    void shutdown();

    // Client management (internal API - use MidiClient class)
    void registerClient(
        void* clientId,
        const MidiClientState& state,
        int queueSize,
        std::shared_ptr<MidiMessageQueue>& outIncomingQueue,
        std::shared_ptr<MidiMessageQueue>& outOutgoingQueue
    );
    void unregisterClient(void* clientId);
    void updateClientSubscriptions(void* clientId, const MidiClientState& state);
    MidiClientState getClientState(void* clientId) const;

    juce::StringArray getAvailableMidiInputDevices() const;
    juce::StringArray getAvailableMidiOutputDevices() const;

    juce::AudioDeviceManager& getAudioDeviceManager()
    {
        return deviceManager;
    }

private:
    MidiServer();

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void timerCallback() override;
    void updateMidiDeviceSubscriptions();
    void rebuildClientSnapshot();

    struct ClientInfo
    {
        MidiClientState state;
        std::shared_ptr<MidiMessageQueue> incomingMidiQueue;
        std::shared_ptr<MidiMessageQueue> outgoingMidiQueue;
    };

    struct ClientSnapshot
    {
        std::shared_ptr<MidiMessageQueue> incomingMidiQueue;
        std::shared_ptr<MidiMessageQueue> outgoingMidiQueue;
        MidiClientState state;
    };

    struct DeviceSnapshot
    {
        std::unordered_map<juce::String, std::vector<ClientSnapshot>> inputSubscriptions;
        std::unordered_map<juce::String, std::vector<ClientSnapshot>> outputSubscriptions;
    };

    juce::AudioDeviceManager deviceManager;
    mutable juce::CriticalSection clientsMutex;
    std::unordered_map<void*, ClientInfo> clients;
    AtomicSharedPtr<DeviceSnapshot> activeSnapshot{std::make_shared<DeviceSnapshot>()};
    juce::HashMap<juce::String, juce::MidiOutput*> outputDevices;
    bool initialized = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiServer)
};

} // namespace atk
