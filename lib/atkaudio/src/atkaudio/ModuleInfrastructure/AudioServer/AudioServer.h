#pragma once

#include <atkaudio/AtomicSharedPtr.h>
#include <atkaudio/FifoBuffer2.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include <unordered_map>

namespace atk
{

// Forward declaration
class AudioServer;

/**
 * Per-channel subscription info
 */
struct ChannelSubscription
{
    juce::String deviceName;
    juce::String deviceType; // e.g., "Windows Audio", "ASIO", "Windows Audio (Exclusive Mode)"
    int channelIndex = -1;
    bool isInput = true;

    bool operator==(const ChannelSubscription& other) const
    {
        return deviceName == other.deviceName
            && deviceType == other.deviceType
            && channelIndex == other.channelIndex
            && isInput == other.isInput;
    }

    juce::String toString() const
    {
        return deviceType + "|" + deviceName + ":" + juce::String(channelIndex) + ":" + (isInput ? "in" : "out");
    }

    static ChannelSubscription fromString(const juce::String& str)
    {
        // Support both old format (no device type) and new format (with device type)
        ChannelSubscription sub;

        if (str.contains("|"))
        {
            // New format: deviceType|deviceName:channelIndex:in/out
            auto parts = juce::StringArray::fromTokens(str, "|", "");
            if (parts.size() >= 2)
            {
                sub.deviceType = parts[0];
                auto tokens = juce::StringArray::fromTokens(parts[1], ":", "");
                if (tokens.size() >= 3)
                {
                    sub.deviceName = tokens[0];
                    sub.channelIndex = tokens[1].getIntValue();
                    sub.isInput = tokens[2] == "in";
                }
            }
        }
        else
        {
            // Old format (backward compatibility): deviceName:channelIndex:in/out
            auto tokens = juce::StringArray::fromTokens(str, ":", "");
            if (tokens.size() >= 3)
            {
                sub.deviceName = tokens[0];
                sub.channelIndex = tokens[1].getIntValue();
                sub.isInput = tokens[2] == "in";
                // deviceType remains empty for old format
            }
        }
        return sub;
    }
};

/**
 * Channel mapping: maps a subscribed device channel to a client channel
 */
struct ChannelMapping
{
    ChannelSubscription deviceChannel;
    int clientChannel = -1;

    juce::String serialize() const
    {
        return deviceChannel.toString() + ">" + juce::String(clientChannel);
    }

    static ChannelMapping deserialize(const juce::String& str)
    {
        auto parts = juce::StringArray::fromTokens(str, ">", "");
        ChannelMapping mapping;
        if (parts.size() >= 2)
        {
            mapping.deviceChannel = ChannelSubscription::fromString(parts[0]);
            mapping.clientChannel = parts[1].getIntValue();
        }
        return mapping;
    }
};

/**
 * Client subscription state - stores which device channels the client is subscribed to
 */
struct AudioClientState
{
    std::vector<ChannelSubscription> inputSubscriptions;
    std::vector<ChannelSubscription> outputSubscriptions;

    bool operator==(const AudioClientState& other) const
    {
        return inputSubscriptions == other.inputSubscriptions && outputSubscriptions == other.outputSubscriptions;
    }

    bool operator!=(const AudioClientState& other) const
    {
        return !(*this == other);
    }

    juce::String serialize() const;
    void deserialize(const juce::String& data);
};

/**
 * Audio Client handle - use composition instead of inheritance
 * Manages registration with AudioServer and provides access to device channel subscriptions
 *
 * NEW ARCHITECTURE:
 * - Client subscribes to device channels
 * - Server provides multichannel buffer access (one channel per subscription)
 * - Client applies its own routing/mixing logic
 *
 * Usage:
 *   class MyAudioProcessor {
 *       atk::AudioClient audioClient;
 *       AudioBuffer<float> deviceInputBuffer;  // One channel per input subscription
 *       AudioBuffer<float> deviceOutputBuffer; // One channel per output subscription
 *
 *       void processBlock(AudioBuffer<float>& pluginBuffer) {
 *           // Pull all subscribed inputs (server fills one channel per subscription)
 *           audioClient.pullSubscribedInputs(deviceInputBuffer, numSamples, sampleRate);
 *
 *           // Apply input routing matrix: deviceInputBuffer -> pluginBuffer
 *           for (int sub = 0; sub < audioClient.getSubscriptions().inputSubscriptions.size(); ++sub) {
 *               for (int pluginCh = 0; pluginCh < pluginBuffer.getNumChannels(); ++pluginCh) {
 *                   if (inputRoutingMatrix[sub][pluginCh]) {
 *                       pluginBuffer.addFrom(pluginCh, 0, deviceInputBuffer, sub, 0, numSamples);
 *                   }
 *               }
 *           }
 *
 *           // ... process plugin...
 *
 *           // Apply output routing matrix: pluginBuffer -> deviceOutputBuffer
 *           deviceOutputBuffer.clear();
 *           for (int pluginCh = 0; pluginCh < pluginBuffer.getNumChannels(); ++pluginCh) {
 *               for (int sub = 0; sub < audioClient.getSubscriptions().outputSubscriptions.size(); ++sub) {
 *                   if (outputRoutingMatrix[pluginCh][sub]) {
 *                       deviceOutputBuffer.addFrom(sub, 0, pluginBuffer, pluginCh, 0, numSamples);
 *                   }
 *               }
 *           }
 *
 *           // Push all subscribed outputs (server sends one channel per subscription)
 *           audioClient.pushSubscribedOutputs(deviceOutputBuffer, numSamples, sampleRate);
 *       }
 *   };
 */
class AudioClient
{
public:
    AudioClient(int bufferSize = 8192);
    ~AudioClient();

    // Non-copyable, movable
    AudioClient(const AudioClient&) = delete;
    AudioClient& operator=(const AudioClient&) = delete;
    AudioClient(AudioClient&&) noexcept;
    AudioClient& operator=(AudioClient&&) noexcept;

    /**
     * Pull audio from all subscribed input devices
     * @param deviceBuffer Buffer to fill (one channel per input subscription)
     * @param numSamples Number of samples to read
     * @param sampleRate Client's sample rate
     */
    void pullSubscribedInputs(juce::AudioBuffer<float>& deviceBuffer, int numSamples, double sampleRate);

    /**
     * Push audio to all subscribed output devices
     * @param deviceBuffer Buffer containing audio (one channel per output subscription)
     * @param numSamples Number of samples to write
     * @param sampleRate Client's sample rate
     */
    void pushSubscribedOutputs(const juce::AudioBuffer<float>& deviceBuffer, int numSamples, double sampleRate);

    /**
     * Update device/channel subscriptions
     */
    void setSubscriptions(const AudioClientState& state);

    /**
     * Get current subscriptions
     */
    AudioClientState getSubscriptions() const;

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
 * Per-device audio manager
 * Each device gets its own AudioDeviceManager and callback
 * Operates in FULL-DUPLEX mode (both input and output) for reliable callbacks
 */
class AudioDeviceHandler
    : public juce::AudioIODeviceCallback
    , public juce::ChangeListener
{
    friend class AudioServer; // Allow AudioServer to access private members

public:
    AudioDeviceHandler(const juce::String& deviceName);
    ~AudioDeviceHandler() override;

    bool openDevice(const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup);
    void closeDevice();
    bool isDeviceOpen() const;

    // AudioIODeviceCallback interface
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context
    ) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // ChangeListener interface - detects device configuration changes
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    // Client subscription management
    void addClientSubscription(void* clientId, const std::vector<ChannelSubscription>& subscriptions, bool isInput);
    void removeClientSubscription(void* clientId, bool isInput);
    bool hasActiveSubscriptions() const;

    // Direct callback registration (for PluginHost2)
    // Allows a client to register a callback that runs directly in the device callback thread.
    // The direct callback processes in parallel with subscription-based routing:
    //   - Both receive the SAME clean/unprocessed input from the device
    //   - Each direct callback writes to its own separate temporary output buffer
    //   - All outputs (subscriptions + direct callbacks) are SUMMED together into final device output
    // This allows both subscription routing and direct processing to coexist and mix their outputs.
    // Multiple direct callbacks can be registered per device (each gets its own temp buffer).
    // Returns true if successfully registered, false if the same callback is already registered.
    bool registerDirectCallback(juce::AudioIODeviceCallback* callback);
    void unregisterDirectCallback(juce::AudioIODeviceCallback* callback);
    bool hasDirectCallback() const;

    juce::String getDeviceName() const
    {
        return deviceName;
    }

    int getNumChannels() const;
    double getSampleRate() const;
    int getBufferSize() const;

private:
    struct ClientBuffers
    {
        // Multichannel SyncBuffers for sample rate conversion (one per device-client pair)
        // This ensures all channels stay synchronized during drift correction
        std::shared_ptr<SyncBuffer> inputBuffer;  // Multichannel: device -> client
        std::shared_ptr<SyncBuffer> outputBuffer; // Multichannel: client -> device
        std::vector<ChannelMapping> inputMappings;
        std::vector<ChannelMapping> outputMappings;
    };

    // Lock-free snapshot for audio callback
    struct ClientBuffersSnapshot
    {
        std::shared_ptr<SyncBuffer> inputBuffer;  // Multichannel: device -> client
        std::shared_ptr<SyncBuffer> outputBuffer; // Multichannel: client -> device
        std::vector<ChannelMapping> inputMappings;
        std::vector<ChannelMapping> outputMappings;
    };

    struct DeviceSnapshot
    {
        std::unordered_map<void*, ClientBuffersSnapshot> clients;
    };

    juce::String deviceName;
    std::unique_ptr<juce::AudioDeviceManager> deviceManager;

    // Mutable state (protected by mutex, modified by non-RT threads)
    std::unordered_map<void*, ClientBuffers> clientBuffers;
    mutable std::mutex clientBuffersMutex;

    // Lock-free snapshot (read by audio callback without blocking)
    AtomicSharedPtr<DeviceSnapshot> activeSnapshot{std::make_shared<DeviceSnapshot>()};

    // Direct callbacks (for PluginHost2) - each gets its own temp buffer
    struct DirectCallbackInfo
    {
        juce::AudioIODeviceCallback* callback;
        juce::AudioBuffer<float> tempOutputBuffer; // Each callback gets own buffer
        std::vector<float*> outputPointers;        // Pre-allocated pointer array
    };

    std::unordered_map<juce::AudioIODeviceCallback*, DirectCallbackInfo> directCallbacks;
    mutable std::mutex directCallbackMutex;

    // Lock-free snapshot for direct callbacks (read by audio callback)
    struct DirectCallbackSnapshot
    {
        std::vector<DirectCallbackInfo*> callbacks;
    };

    AtomicSharedPtr<DirectCallbackSnapshot> directCallbackSnapshot{std::make_shared<DirectCallbackSnapshot>()};

    // Pre-allocated buffers for real-time safe subscription processing
    // These are resized in audioDeviceAboutToStart() to avoid allocations in the callback
    juce::AudioBuffer<float> rtSubscriptionTempBuffer; // For subscription output reads
    std::vector<float*> rtSubscriptionPointers;        // Pointers for subscription reads
    std::vector<const float*> rtInputPointers;         // Pointers for input writes

    // Helper to rebuild snapshot after subscription changes
    void rebuildSnapshotLocked();
    std::shared_ptr<DeviceSnapshot> getSnapshot() const;

    // Helper to rebuild direct callback snapshot
    void rebuildDirectCallbackSnapshotLocked();

    std::atomic<bool> isRunning{false};
};

/**
 * Global Audio Server singleton
 * Centralized audio input/output service using multiple juce::AudioDeviceManagers
 * Routes audio traffic between physical devices and clients
 */
class AudioServer
    : public juce::DeletedAtShutdown
    , public juce::Timer
{
public:
    JUCE_DECLARE_SINGLETON(AudioServer, false)

    ~AudioServer() override;

    /**
     * Initialize the audio server (called at plugin load)
     */
    void initialize();

    /**
     * Shutdown the audio server (called at plugin unload)
     */
    void shutdown();

    /**
     * Register a client with the server (internal API)
     * @param clientId Unique client identifier
     * @param state The client's subscription state
     * @param bufferSize The size of the client's audio buffers (default: 8192)
     */
    void registerClient(void* clientId, const AudioClientState& state, int bufferSize = 8192);

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
    void updateClientSubscriptions(void* clientId, const AudioClientState& state);

    /**
     * Get a client's current subscription state (internal API)
     * @param clientId Unique client identifier
     * @return The client's subscription state
     */
    AudioClientState getClientState(void* clientId) const;

    /**
     * Pull audio from all subscribed input devices for a client (internal API)
     * Fills buffer with audio from all input subscriptions
     * @param clientId Unique client identifier
     * @param deviceBuffer The buffer to fill (one channel per subscription)
     * @param numSamples Number of samples to read
     * @param sampleRate Client's sample rate for resampling
     */
    void
    pullSubscribedInputs(void* clientId, juce::AudioBuffer<float>& deviceBuffer, int numSamples, double sampleRate);

    /**
     * Push audio to all subscribed output devices for a client (internal API)
     * Sends audio from buffer to all output subscriptions
     * @param clientId Unique client identifier
     * @param deviceBuffer The buffer containing audio (one channel per subscription)
     * @param numSamples Number of samples to write
     * @param sampleRate Client's sample rate for resampling
     */
    void pushSubscribedOutputs(
        void* clientId,
        const juce::AudioBuffer<float>& deviceBuffer,
        int numSamples,
        double sampleRate
    );

    /**
     * Get list of available input devices
     */
    juce::StringArray getAvailableInputDevices() const;

    /**
     * Get list of available output devices
     */
    juce::StringArray getAvailableOutputDevices() const;

    /**
     * Get list of available device types (ASIO, WASAPI, etc.) with their input devices
     * @return Map of device type name -> array of input device names
     */
    std::map<juce::String, juce::StringArray> getInputDevicesByType() const;

    /**
     * Get list of available device types (ASIO, WASAPI, etc.) with their output devices
     * @return Map of device type name -> array of output device names
     */
    std::map<juce::String, juce::StringArray> getOutputDevicesByType() const;

    /**
     * Get number of channels for a device
     */
    int getDeviceNumChannels(const juce::String& deviceName, bool isInput) const;

    /**
     * Get the names of input or output channels available on a device.
     * Results are cached to avoid repeatedly opening devices.
     * @param deviceName Name of the device
     * @param isInput True for input channels, false for output channels
     * @return Array of channel names
     */
    juce::StringArray getDeviceChannelNames(const juce::String& deviceName, bool isInput) const;

    /**
     * Get available sample rates for a device
     * Results are cached to avoid repeatedly opening devices.
     * @param deviceName Name of the device
     * @return Array of available sample rates in Hz
     */
    juce::Array<double> getAvailableSampleRates(const juce::String& deviceName) const;

    /**
     * Get available buffer sizes for a device
     * Results are cached to avoid repeatedly opening devices.
     * @param deviceName Name of the device
     * @return Array of available buffer sizes in samples
     */
    juce::Array<int> getAvailableBufferSizes(const juce::String& deviceName) const;

    /**
     * Change sample rate for an open device
     * @param deviceName Name of the device
     * @param newSampleRate Desired sample rate in Hz
     * @return True if successful, false if device not open or sample rate not supported
     */
    bool setDeviceSampleRate(const juce::String& deviceName, double newSampleRate);

    /**
     * Change buffer size for an open device
     * @param deviceName Name of the device
     * @param newBufferSize Desired buffer size in samples
     * @return True if successful, false if device not open or buffer size not supported
     */
    bool setDeviceBufferSize(const juce::String& deviceName, int newBufferSize);

    /**
     * Cache device information to avoid creating temp devices later.
     * Called when a device is opened to populate the cache.
     * @param deviceName Name of the device
     * @param inputChannelNames Input channel names
     * @param outputChannelNames Output channel names
     * @param sampleRates Available sample rates
     * @param bufferSizes Available buffer sizes
     */
    void cacheDeviceInfo(
        const juce::String& deviceName,
        const juce::StringArray& inputChannelNames,
        const juce::StringArray& outputChannelNames,
        const juce::Array<double>& sampleRates,
        const juce::Array<int>& bufferSizes
    );

    /**
     * Invalidate cached device information for a specific device.
     * Called when device configuration changes to force re-querying capabilities.
     * @param deviceName Name of the device to invalidate cache for
     */
    void invalidateDeviceCache(const juce::String& deviceName);

    /**
     * Get current sample rate for a device (if device is open)
     * @param deviceName Name of the device
     * @return Current sample rate, or 0.0 if device not found/open
     */
    double getCurrentSampleRate(const juce::String& deviceName) const;

    /**
     * Get current buffer size for a device (if device is open)
     * @param deviceName Name of the device
     * @return Current buffer size in samples, or 0 if device not found/open
     */
    int getCurrentBufferSize(const juce::String& deviceName) const;

    /**
     * Get current audio device setup for a device (if device is open)
     * @param deviceName Name of the device
     * @param outSetup Output parameter that will be filled with current setup
     * @return true if device found and setup retrieved, false otherwise
     */
    bool
    getCurrentDeviceSetup(const juce::String& deviceName, juce::AudioDeviceManager::AudioDeviceSetup& outSetup) const;

    /**
     * Get device handler for a device (internal use by settings component)
     * @param deviceName Name of the device
     * @return Pointer to device handler, or nullptr if not found
     */
    AudioDeviceHandler* getDeviceHandler(const juce::String& deviceName) const;

    /**
     * Register a direct callback for a device (for PluginHost2)
     * This allows the callback to run directly in the device's audio thread.
     * The direct callback processes in parallel with subscription-based routing:
     *   - Both receive the SAME clean/unprocessed input from the device
     *   - Each direct callback writes to its own separate temporary output buffer
     *   - All outputs (subscriptions + direct callbacks) are SUMMED together into final device output
     * This allows both subscription routing and direct processing to coexist and mix their outputs.
     * Multiple direct callbacks can be registered per device (each gets its own temp buffer).
     * The device will be opened if not already open.
     *
     * @param deviceName Name of the device to register with
     * @param callback The callback to register
     * @param preferredSetup Preferred device settings (sample rate, buffer size, channels)
     * @return True if successfully registered, false if another callback is already registered
     */
    bool registerDirectCallback(
        const juce::String& deviceName,
        juce::AudioIODeviceCallback* callback,
        const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup
    );

    /**
     * Unregister a direct callback from a device
     * @param deviceName Name of the device to unregister from
     * @param callback The callback to unregister
     */
    void unregisterDirectCallback(const juce::String& deviceName, juce::AudioIODeviceCallback* callback);

    /**
     * Check if a device has a direct callback registered
     * @param deviceName Name of the device to check
     * @return True if a direct callback is registered
     */
    bool hasDirectCallback(const juce::String& deviceName) const;

private:
    AudioServer();

    // Timer callback for deferred device closing
    void timerCallback() override;

    // Cancel pending close for a device (called when reused before close timer expires)
    void cancelPendingDeviceClose(const juce::String& deviceName);

    // Schedule device for deferred closing
    void scheduleDeviceClose(const juce::String& deviceName);

    struct ClientInfo
    {
        std::atomic<AudioClientState*> state{nullptr}; // Lock-free atomic pointer
        AudioClientState* pendingDeleteState{nullptr}; // Deferred deletion for safety
        juce::CriticalSection stateUpdateMutex;        // Only for UI thread updates
        int bufferSize = 8192;

        // Default constructor
        ClientInfo() = default;

        // Move constructor
        ClientInfo(ClientInfo&& other) noexcept
            : state(other.state.exchange(nullptr, std::memory_order_acquire))
            , pendingDeleteState(other.pendingDeleteState)
            , bufferSize(other.bufferSize)
        {
            other.pendingDeleteState = nullptr;
        }

        // Move assignment
        ClientInfo& operator=(ClientInfo&& other) noexcept
        {
            if (this != &other)
            {
                // Clean up existing state
                delete state.load(std::memory_order_acquire);
                delete pendingDeleteState;

                // Move from other
                state.store(other.state.exchange(nullptr, std::memory_order_acquire), std::memory_order_release);
                pendingDeleteState = other.pendingDeleteState;
                bufferSize = other.bufferSize;

                other.pendingDeleteState = nullptr;
            }
            return *this;
        }

        // Delete copy operations
        ClientInfo(const ClientInfo&) = delete;
        ClientInfo& operator=(const ClientInfo&) = delete;

        ~ClientInfo()
        {
            delete state.load(std::memory_order_acquire);
            delete pendingDeleteState;
        }
    };

    // Get or create device handler (opens device on first subscription)
    AudioDeviceHandler* getOrCreateDeviceHandler(const juce::String& deviceName);

    // Remove device handler if no active subscriptions (closes device)
    void removeDeviceHandlerIfUnused(const juce::String& deviceName);

    mutable std::mutex clientsMutex;
    std::unordered_map<void*, ClientInfo> clients;

    mutable std::mutex devicesMutex;
    // Single handler per device (full-duplex operation)
    std::unordered_map<juce::String, std::unique_ptr<AudioDeviceHandler>> deviceHandlers;

    // Deferred device closing: track devices pending closure
    struct PendingDeviceClose
    {
        juce::String deviceName;
        juce::int64 closeTime; // Time in milliseconds when device should be closed
    };

    std::vector<PendingDeviceClose> pendingDeviceCloses;

    // Global device enumerator for querying available devices (lazily initialized)
    mutable std::mutex deviceEnumeratorMutex;
    mutable std::unique_ptr<juce::AudioDeviceManager> deviceEnumerator;

    // Cache for device channel counts to avoid repeatedly opening devices
    mutable std::mutex deviceChannelCacheMutex;
    mutable std::unordered_map<juce::String, int> inputDeviceChannelCache;
    mutable std::unordered_map<juce::String, int> outputDeviceChannelCache;

    // Cache for device channel names
    mutable std::unordered_map<juce::String, juce::StringArray> inputDeviceChannelNamesCache;
    mutable std::unordered_map<juce::String, juce::StringArray> outputDeviceChannelNamesCache;

    // Cache for device capabilities to avoid repeatedly opening devices
    mutable std::mutex deviceCapabilitiesCacheMutex;
    mutable std::unordered_map<juce::String, juce::Array<double>> deviceSampleRatesCache;
    mutable std::unordered_map<juce::String, juce::Array<int>> deviceBufferSizesCache;

    std::atomic<bool> initialized{false};

    // Helper to lazily initialize device enumerator with thread safety
    juce::AudioDeviceManager* ensureDeviceEnumerator() const;
};

} // namespace atk
