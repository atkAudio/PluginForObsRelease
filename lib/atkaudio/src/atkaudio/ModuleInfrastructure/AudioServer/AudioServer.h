#pragma once

#include <atkaudio/AtomicSharedPtr.h>
#include <atkaudio/FifoBuffer2.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include <set>
#include <unordered_map>

namespace atk
{

class AudioServer;
class AudioClient;

class AudioDeviceEnumerator
{
public:
    static juce::StringArray getAvailableInputDevices();
    static juce::StringArray getAvailableOutputDevices();
    static std::map<juce::String, juce::StringArray> getInputDevicesByType();
    static std::map<juce::String, juce::StringArray> getOutputDevicesByType();
    static int getDeviceNumChannels(const juce::String& deviceName, bool isInput);
    static juce::StringArray getDeviceChannelNames(const juce::String& deviceName, bool isInput);
    static juce::Array<double> getAvailableSampleRates(const juce::String& deviceName);
    static juce::Array<int> getAvailableBufferSizes(const juce::String& deviceName);

private:
    static juce::AudioDeviceManager* ensureEnumerator();
    static std::mutex enumeratorMutex;
    static std::unique_ptr<juce::AudioDeviceManager> enumerator;
    static std::mutex cacheMutex;
    static std::unordered_map<juce::String, int> inputChannelCache;
    static std::unordered_map<juce::String, int> outputChannelCache;
    static std::unordered_map<juce::String, juce::StringArray> inputChannelNamesCache;
    static std::unordered_map<juce::String, juce::StringArray> outputChannelNamesCache;
    static std::unordered_map<juce::String, juce::Array<double>> sampleRatesCache;
    static std::unordered_map<juce::String, juce::Array<int>> bufferSizesCache;
};

struct ChannelSubscription
{
    juce::String deviceName;
    juce::String deviceType; // "Windows Audio", "ASIO", etc.
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
        ChannelSubscription sub;
        if (str.contains("|"))
        {
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
            auto tokens = juce::StringArray::fromTokens(str, ":", "");
            if (tokens.size() >= 3)
            {
                sub.deviceName = tokens[0];
                sub.channelIndex = tokens[1].getIntValue();
                sub.isInput = tokens[2] == "in";
            }
        }
        return sub;
    }
};

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

class AudioClient
{
public:
    AudioClient(int bufferSize = 8192);
    ~AudioClient();

    AudioClient(const AudioClient&) = delete;
    AudioClient& operator=(const AudioClient&) = delete;
    AudioClient(AudioClient&&) noexcept;
    AudioClient& operator=(AudioClient&&) noexcept;

    void pullSubscribedInputs(juce::AudioBuffer<float>& deviceBuffer, int numSamples, double sampleRate);
    void pushSubscribedOutputs(const juce::AudioBuffer<float>& deviceBuffer, int numSamples, double sampleRate);
    void setSubscriptions(const AudioClientState& state);
    AudioClientState getSubscriptions() const;

    void* getClientId() const
    {
        return clientId;
    }

    int getNumInputSubscriptions() const;
    int getNumOutputSubscriptions() const;

private:
    friend class AudioServer;

    void* clientId;
    int clientBufferSize;

    struct ChannelBufferRef
    {
        ChannelSubscription subscription;
        std::shared_ptr<SyncBuffer> buffer;
        int deviceChannelIndex = 0;
    };

    struct BufferGroup
    {
        SyncBuffer* buffer = nullptr;
        int maxDeviceChannel = 0;
        std::vector<std::pair<int, int>> channelMap;
    };

    struct BufferSnapshot
    {
        std::vector<ChannelBufferRef> inputBuffers;
        std::vector<ChannelBufferRef> outputBuffers;
        std::vector<BufferGroup> inputGroups;
        std::vector<BufferGroup> outputGroups;
        AudioClientState state;
    };

    AtomicSharedPtr<BufferSnapshot> bufferSnapshot{std::make_shared<BufferSnapshot>()};
    juce::AudioBuffer<float> tempInputBuffer;
    juce::AudioBuffer<float> tempOutputBuffer;
    std::vector<float*> tempInputPointers;
    std::vector<const float*> tempOutputPointers;

    void updateBufferSnapshot(std::shared_ptr<BufferSnapshot> newSnapshot);
    void ensureTempBufferCapacity(int numChannels, int numSamples);
};

class AudioDeviceHandler
    : public juce::AudioIODeviceCallback
    , public juce::ChangeListener
{
    friend class AudioServer;

public:
    AudioDeviceHandler(const juce::String& deviceName);
    ~AudioDeviceHandler() override;

    bool openDevice(const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup);
    void closeDevice();
    bool isDeviceOpen() const;

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
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    void addClientSubscription(void* clientId, const std::vector<ChannelSubscription>& subscriptions, bool isInput);
    void removeClientSubscription(void* clientId, bool isInput);
    bool hasActiveSubscriptions() const;

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
        std::shared_ptr<SyncBuffer> inputBuffer;
        std::shared_ptr<SyncBuffer> outputBuffer;
        std::vector<ChannelMapping> inputMappings;
        std::vector<ChannelMapping> outputMappings;
    };

    struct ClientBuffersSnapshot
    {
        std::shared_ptr<SyncBuffer> inputBuffer;
        std::shared_ptr<SyncBuffer> outputBuffer;
        std::vector<ChannelMapping> inputMappings;
        std::vector<ChannelMapping> outputMappings;
    };

    struct DeviceSnapshot
    {
        std::unordered_map<void*, ClientBuffersSnapshot> clients;
    };

    struct DirectCallbackInfo
    {
        juce::AudioIODeviceCallback* callback;
        juce::AudioBuffer<float> tempOutputBuffer;
        std::vector<float*> outputPointers;
    };

    struct DirectCallbackSnapshot
    {
        std::vector<DirectCallbackInfo*> callbacks;
    };

    juce::String deviceName;
    std::unique_ptr<juce::AudioDeviceManager> deviceManager;

    std::unordered_map<void*, ClientBuffers> clientBuffers;
    mutable std::mutex clientBuffersMutex;
    AtomicSharedPtr<DeviceSnapshot> activeSnapshot{std::make_shared<DeviceSnapshot>()};

    std::unordered_map<juce::AudioIODeviceCallback*, DirectCallbackInfo> directCallbacks;
    mutable std::mutex directCallbackMutex;
    AtomicSharedPtr<DirectCallbackSnapshot> directCallbackSnapshot{std::make_shared<DirectCallbackSnapshot>()};

    juce::AudioBuffer<float> rtSubscriptionTempBuffer;
    std::vector<float*> rtSubscriptionPointers;
    std::vector<const float*> rtInputPointers;

    void rebuildSnapshotLocked();
    std::shared_ptr<DeviceSnapshot> getSnapshot() const;
    void rebuildDirectCallbackSnapshotLocked();

    std::atomic<bool> isRunning{false};
};

class AudioServer
    : public juce::DeletedAtShutdown
    , private juce::ChangeListener
{
public:
    JUCE_DECLARE_SINGLETON(AudioServer, false)
    ~AudioServer() override;

    // Listener interface for device list changes
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void audioServerDeviceListChanged() = 0;
    };

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void initialize();
    void shutdown();

    void registerClient(void* clientId, const AudioClientState& state, int bufferSize = 8192);
    void unregisterClient(void* clientId);
    void updateClientSubscriptions(void* clientId, const AudioClientState& state);
    AudioClientState getClientState(void* clientId) const;

    juce::StringArray getAvailableInputDevices() const;
    juce::StringArray getAvailableOutputDevices() const;
    std::map<juce::String, juce::StringArray> getInputDevicesByType() const;
    std::map<juce::String, juce::StringArray> getOutputDevicesByType() const;

    int getDeviceNumChannels(const juce::String& deviceName, bool isInput) const;
    juce::StringArray getDeviceChannelNames(const juce::String& deviceName, bool isInput) const;
    juce::Array<double> getAvailableSampleRates(const juce::String& deviceName) const;
    juce::Array<int> getAvailableBufferSizes(const juce::String& deviceName) const;

    bool setDeviceSampleRate(const juce::String& deviceName, double newSampleRate);
    bool setDeviceBufferSize(const juce::String& deviceName, int newBufferSize);
    double getCurrentSampleRate(const juce::String& deviceName) const;
    int getCurrentBufferSize(const juce::String& deviceName) const;
    bool
    getCurrentDeviceSetup(const juce::String& deviceName, juce::AudioDeviceManager::AudioDeviceSetup& outSetup) const;

    void cacheDeviceInfo(
        const juce::String& deviceName,
        const juce::StringArray& inputChannelNames,
        const juce::StringArray& outputChannelNames,
        const juce::Array<double>& sampleRates,
        const juce::Array<int>& bufferSizes
    );
    void invalidateDeviceCache(const juce::String& deviceName);

    bool registerDirectCallback(
        const juce::String& deviceName,
        juce::AudioIODeviceCallback* callback,
        const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup
    );
    void unregisterDirectCallback(const juce::String& deviceName, juce::AudioIODeviceCallback* callback);
    bool hasDirectCallback(const juce::String& deviceName) const;

    AudioDeviceHandler* getDeviceHandler(const juce::String& deviceName) const;

private:
    AudioServer();

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void setupDeviceEnumeratorListeners();

    void processDeviceCleanup();
    void scheduleDeviceClose(const juce::String& deviceKey);
    bool cancelPendingDeviceClose(const juce::String& deviceKey);
    static juce::String makeDeviceKey(const juce::String& deviceType, const juce::String& deviceName);
    static juce::String makeDeviceKey(const ChannelSubscription& sub);
    juce::String findDeviceKeyByName(const juce::String& deviceName) const;
    void rebuildClientBufferSnapshot(void* clientId);
    AudioDeviceHandler* getOrCreateDeviceHandler(const juce::String& deviceKey);
    void removeDeviceHandlerIfUnused(const juce::String& deviceKey);
    juce::AudioDeviceManager* ensureDeviceEnumerator() const;

    struct ClientInfo
    {
        AudioClient* clientPtr = nullptr;
        AtomicSharedPtr<AudioClientState> state{std::make_shared<AudioClientState>()};
        int bufferSize = 8192;

        ClientInfo() = default;

        ClientInfo(ClientInfo&& other) noexcept
            : clientPtr(other.clientPtr)
            , bufferSize(other.bufferSize)
        {
            // Transfer state ownership
            auto otherState = other.state.load();
            state.store(std::move(otherState));
            other.clientPtr = nullptr;
        }

        ClientInfo& operator=(ClientInfo&& other) noexcept
        {
            if (this != &other)
            {
                clientPtr = other.clientPtr;
                bufferSize = other.bufferSize;
                auto otherState = other.state.load();
                state.store(std::move(otherState));
                other.clientPtr = nullptr;
            }
            return *this;
        }

        ClientInfo(const ClientInfo&) = delete;
        ClientInfo& operator=(const ClientInfo&) = delete;
    };

    struct PendingDeviceClose
    {
        juce::String deviceKey;
        juce::int64 closeTime;
    };

    mutable std::mutex clientsMutex;
    std::unordered_map<void*, ClientInfo> clients;

    mutable std::mutex devicesMutex;
    std::unordered_map<juce::String, std::unique_ptr<AudioDeviceHandler>> deviceHandlers;
    std::vector<PendingDeviceClose> pendingDeviceCloses;
    static constexpr juce::int64 DEVICE_CLOSE_DELAY_MS = 5000;

    mutable std::mutex deviceEnumeratorMutex;
    mutable std::unique_ptr<juce::AudioDeviceManager> deviceEnumerator;

    mutable std::mutex deviceChannelCacheMutex;
    mutable std::unordered_map<juce::String, int> inputDeviceChannelCache;
    mutable std::unordered_map<juce::String, int> outputDeviceChannelCache;
    mutable std::unordered_map<juce::String, juce::StringArray> inputDeviceChannelNamesCache;
    mutable std::unordered_map<juce::String, juce::StringArray> outputDeviceChannelNamesCache;

    mutable std::mutex deviceCapabilitiesCacheMutex;
    mutable std::unordered_map<juce::String, juce::Array<double>> deviceSampleRatesCache;
    mutable std::unordered_map<juce::String, juce::Array<int>> deviceBufferSizesCache;

    mutable std::mutex deviceTypeCacheMutex;
    mutable std::unordered_map<juce::String, juce::String> deviceNameToTypeCache;

    juce::ListenerList<Listener> listeners;

    std::atomic<bool> initialized{false};
};

} // namespace atk
