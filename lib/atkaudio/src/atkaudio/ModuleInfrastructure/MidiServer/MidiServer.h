#pragma once

#include <atkaudio/AtomicSharedPtr.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include <unordered_map>

namespace atk
{

// Forward declaration
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

/**
 * MIDI Client handle - use composition instead of inheritance
 * Manages registration with MidiServer and provides MIDI I/O
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
    MidiClient(int queueSize = 65536);
    ~MidiClient();

    // Non-copyable, movable
    MidiClient(const MidiClient&) = delete;
    MidiClient& operator=(const MidiClient&) = delete;
    MidiClient(MidiClient&&) noexcept;
    MidiClient& operator=(MidiClient&&) noexcept;

    /**
     * Get pending MIDI messages for this client
     * Call this from your audio callback
     */
    void getPendingMidi(juce::MidiBuffer& outBuffer, int numSamples, double sampleRate);

    /**
     * Send MIDI messages to subscribed output devices
     * Call this from your audio callback
     */
    void sendMidi(const juce::MidiBuffer& messages);

    /**
     * Inject MIDI messages directly (bypassing physical inputs)
     * Useful for virtual keyboards, test messages, etc.
     */
    void injectMidi(const juce::MidiBuffer& messages);

    /**
     * Update device subscriptions
     */
    void setSubscriptions(const MidiClientState& state);

    /**
     * Get current subscriptions
     */
    MidiClientState getSubscriptions() const;

    /**
     * Get the unique client ID (for internal use)
     */
    void* getClientId() const
    {
        return clientId;
    }

private:
    void* clientId; // Opaque pointer used as unique identifier
};

/**
 * Thread-safe MIDI message queue for real-time safe communication
 * Uses JUCE AbstractFifo with spinlock protection for MPSC (Multi-Producer Single-Consumer) operation
 *
 * Multiple producers (MIDI input thread, UI thread for virtual keyboard) can safely push.
 * Single consumer (audio thread) reads without blocking.
 *
 * Messages pushed with samplePosition=0 automatically get incrementing positions (0,1,2,...)
 * to preserve ordering of rapid MIDI events. The counter resets after each popAll() call.
 */
class MidiMessageQueue
{
public:
    static constexpr int kDefaultQueueSize = 65536;

    struct TimestampedMidiMessage
    {
        juce::MidiMessage message;
        int samplePosition;

        TimestampedMidiMessage()
            : samplePosition(0)
        {
        }

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

    /**
     * Add a MIDI message to the queue (producer side - MPSC safe)
     * Multiple threads can call push() concurrently.
     * Uses spinlock for short critical section (real-time safe with brief spin).
     *
     * If samplePosition is 0, an auto-incrementing position is used to preserve
     * ordering of rapid MIDI messages that arrive between audio callbacks.
     *
     * @return true if message was added, false if queue is full
     */
    bool push(const juce::MidiMessage& message, int samplePosition)
    {
        // Spinlock to serialize multiple producers
        // The critical section is very short (just FIFO index update + message copy)
        juce::SpinLock::ScopedLockType lock(producerLock);

        int start1, size1, start2, size2;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            messages[start1].message = message;
            // For messages with position 0, use auto-incrementing position to preserve order
            // This ensures rapid note-on/note-off pairs don't get identical timestamps
            if (samplePosition == 0)
                messages[start1].samplePosition = autoIncrementPosition++;
            else
                messages[start1].samplePosition = samplePosition;
            fifo.finishedWrite(1);
            return true;
        }

        return false; // Queue full
    }

    /**
     * Read all pending MIDI messages into a buffer (consumer side - audio thread)
     * @param outBuffer The buffer to fill with MIDI messages
     * @param maxSamples Filter to only include messages within [0, maxSamples)
     */
    void popAll(juce::MidiBuffer& outBuffer, int maxSamples = 65536)
    {
        // Reset auto-increment counter for next batch of messages
        // This ensures each audio callback starts fresh from position 0
        {
            juce::SpinLock::ScopedLockType lock(producerLock);
            autoIncrementPosition = 0;
        }

        int start1, size1, start2, size2;
        const int numReady = fifo.getNumReady();

        if (numReady > 0)
        {
            fifo.prepareToRead(numReady, start1, size1, start2, size2);

            // Read first block
            for (int i = 0; i < size1; ++i)
            {
                const auto& msg = messages[start1 + i];
                // Always add all messages, but filter by sample position if needed
                if (msg.samplePosition < maxSamples)
                    outBuffer.addEvent(msg.message, msg.samplePosition);
                // Note: Messages outside maxSamples are discarded (not re-queued)
            }

            // Read second block (if wrapped)
            for (int i = 0; i < size2; ++i)
            {
                const auto& msg = messages[start2 + i];
                if (msg.samplePosition < maxSamples)
                    outBuffer.addEvent(msg.message, msg.samplePosition);
            }

            // Mark all as read (including filtered ones)
            fifo.finishedRead(size1 + size2);
        }
    }

    /**
     * Clear all pending messages
     */
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

    /**
     * Get the number of messages currently in the queue
     */
    int getNumReady() const
    {
        return fifo.getNumReady();
    }

private:
    juce::SpinLock producerLock;   // Protects push() for MPSC safety
    int autoIncrementPosition = 0; // Auto-incrementing position for rapid MIDI messages
    juce::AbstractFifo fifo;
    std::vector<TimestampedMidiMessage> messages;
};

/**
 * Global MIDI Server singleton
 * Centralized MIDI input/output service using juce::AudioDeviceManager
 * Routes MIDI traffic between physical devices and clients
 */
class MidiServer
    : public juce::DeletedAtShutdown
    , private juce::MidiInputCallback
    , private juce::Timer
{
public:
    JUCE_DECLARE_SINGLETON(MidiServer, false)

    ~MidiServer() override;

    /**
     * Initialize the MIDI server (called at plugin load)
     */
    void initialize();

    /**
     * Shutdown the MIDI server (called at plugin unload)
     */
    void shutdown();

    /**
     * Register a client with the server (internal API)
     * @param clientId Unique client identifier
     * @param state The client's subscription state (which devices to subscribe to)
     * @param queueSize The size of the client's MIDI queue (default: 65536)
     */
    void
    registerClient(void* clientId, const MidiClientState& state, int queueSize = MidiMessageQueue::kDefaultQueueSize);

    /**
     * Unregister a client from the server (internal API)
     * @param clientId Unique client identifier
     */
    void unregisterClient(void* clientId);

    /**
     * Update a client's subscription state (internal API)
     * @param clientId Unique client identifier
     * @param state The new subscription state
     */
    void updateClientSubscriptions(void* clientId, const MidiClientState& state);

    /**
     * Get a client's current subscription state (internal API)
     * @param clientId Unique client identifier
     * @return The client's subscription state
     */
    MidiClientState getClientState(void* clientId) const;

    /**
     * Get pending MIDI messages for a client and clear the buffer (internal API)
     * This should be called by the client in their audio processing callback
     * @param clientId Unique client identifier
     * @param outBuffer The buffer to fill with pending MIDI messages
     * @param numSamples The number of samples in the current processing block
     * @param sampleRate The client's sample rate (for timestamp conversion)
     */
    void
    getPendingMidiForClient(void* clientId, juce::MidiBuffer& outBuffer, int numSamples, double sampleRate = 48000.0);

    /**
     * Send MIDI messages from a client to subscribed output devices (internal API)
     * @param clientId Unique client identifier
     * @param messages The MIDI messages to send
     */
    void sendMidiFromClient(void* clientId, const juce::MidiBuffer& messages);

    /**
     * Inject MIDI messages to a client (for virtual keyboard, etc.) (internal API)
     * @param clientId Unique client identifier
     * @param messages The MIDI messages to inject
     */
    void injectMidiToClient(void* clientId, const juce::MidiBuffer& messages);

    /**
     * Get list of available MIDI input devices
     */
    juce::StringArray getAvailableMidiInputDevices() const;

    /**
     * Get list of available MIDI output devices
     */
    juce::StringArray getAvailableMidiOutputDevices() const;

    /**
     * Get the AudioDeviceManager (for creating settings components)
     */
    juce::AudioDeviceManager& getAudioDeviceManager()
    {
        return deviceManager;
    }

private:
    MidiServer();

    // MidiInputCallback interface
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

    // Timer interface - process outgoing MIDI
    void timerCallback() override;

    // Update MIDI device subscriptions based on all clients' needs
    void updateMidiDeviceSubscriptions();

    // Lock-free snapshot rebuild
    void rebuildClientSnapshot();

    // Internal structures
    struct ClientInfo
    {
        MidiClientState state;
        std::shared_ptr<MidiMessageQueue> incomingMidiQueue; // Lock-free queue for incoming MIDI (shared for snapshot)
        std::shared_ptr<MidiMessageQueue> outgoingMidiQueue; // Lock-free queue for outgoing MIDI (shared for snapshot)
        double sampleRate = 48000.0;                         // Client's sample rate

        ClientInfo()
            : incomingMidiQueue(std::make_shared<MidiMessageQueue>())
            , outgoingMidiQueue(std::make_shared<MidiMessageQueue>())
        {
        }

        ClientInfo(const MidiClientState& s, int queueSize = MidiMessageQueue::kDefaultQueueSize)
            : state(s)
            , incomingMidiQueue(std::make_shared<MidiMessageQueue>(queueSize))
            , outgoingMidiQueue(std::make_shared<MidiMessageQueue>(queueSize))
        {
        }
    };

    // Shared pointer to ClientInfo for lock-free access
    struct ClientSnapshot
    {
        std::shared_ptr<MidiMessageQueue> incomingMidiQueue;
        std::shared_ptr<MidiMessageQueue> outgoingMidiQueue;
        MidiClientState state;
    };

    // Lock-free snapshot: device name -> list of client snapshots
    struct DeviceSnapshot
    {
        std::unordered_map<juce::String, std::vector<ClientSnapshot>> inputSubscriptions;
        std::unordered_map<juce::String, std::vector<ClientSnapshot>> outputSubscriptions;
    };

    juce::AudioDeviceManager deviceManager;
    juce::CriticalSection clientsMutex;
    std::unordered_map<void*, ClientInfo> clients; // Key is client ID (address of MidiClient)

    // Lock-free snapshot for MIDI callback (read without blocking)
    AtomicSharedPtr<DeviceSnapshot> activeSnapshot{std::make_shared<DeviceSnapshot>()};

    // Map of device names to their enabled state
    juce::HashMap<juce::String, bool> enabledInputDevices;
    juce::HashMap<juce::String, juce::MidiOutput*> outputDevices;

    bool initialized = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiServer)
};

} // namespace atk
