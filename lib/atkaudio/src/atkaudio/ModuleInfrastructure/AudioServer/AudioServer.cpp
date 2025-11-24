#include "AudioServer.h"
#include <atkaudio/atkaudio.h>

namespace atk
{

// ============================================================================
// AudioClientState serialization
// ============================================================================

juce::String AudioClientState::serialize() const
{
    juce::StringArray parts;

    // Serialize input subscriptions
    parts.add("IN:" + juce::String(inputSubscriptions.size()));
    for (const auto& sub : inputSubscriptions)
        parts.add(sub.toString());

    // Serialize output subscriptions
    parts.add("OUT:" + juce::String(outputSubscriptions.size()));
    for (const auto& sub : outputSubscriptions)
        parts.add(sub.toString());

    return parts.joinIntoString(";");
}

void AudioClientState::deserialize(const juce::String& data)
{
    inputSubscriptions.clear();
    outputSubscriptions.clear();

    auto parts = juce::StringArray::fromTokens(data, ";", "");

    int index = 0;
    while (index < parts.size())
    {
        if (parts[index].startsWith("IN:"))
        {
            int count = parts[index].fromFirstOccurrenceOf(":", false, false).getIntValue();
            index++;
            for (int i = 0; i < count && index < parts.size(); ++i, ++index)
                inputSubscriptions.push_back(ChannelSubscription::fromString(parts[index]));
        }
        else if (parts[index].startsWith("OUT:"))
        {
            int count = parts[index].fromFirstOccurrenceOf(":", false, false).getIntValue();
            index++;
            for (int i = 0; i < count && index < parts.size(); ++i, ++index)
                outputSubscriptions.push_back(ChannelSubscription::fromString(parts[index]));
        }
        else
        {
            index++;
        }
    }
}

// ============================================================================
// AudioClient implementation
// ============================================================================

AudioClient::AudioClient(int bufferSize)
    : clientId(this)
{
    if (auto* server = AudioServer::getInstanceWithoutCreating())
    {
        AudioClientState emptyState;
        server->registerClient(clientId, emptyState, bufferSize);
    }
}

AudioClient::~AudioClient()
{
    if (auto* server = AudioServer::getInstanceWithoutCreating())
        server->unregisterClient(clientId);
}

AudioClient::AudioClient(AudioClient&& other) noexcept
    : clientId(other.clientId)
{
    other.clientId = nullptr;
}

AudioClient& AudioClient::operator=(AudioClient&& other) noexcept
{
    if (this != &other)
    {
        if (clientId && AudioServer::getInstanceWithoutCreating())
            AudioServer::getInstanceWithoutCreating()->unregisterClient(clientId);

        clientId = other.clientId;
        other.clientId = nullptr;
    }
    return *this;
}

void AudioClient::pullSubscribedInputs(juce::AudioBuffer<float>& deviceBuffer, int numSamples, double sampleRate)
{
    if (auto* server = AudioServer::getInstanceWithoutCreating())
        server->pullSubscribedInputs(clientId, deviceBuffer, numSamples, sampleRate);
}

void AudioClient::pushSubscribedOutputs(const juce::AudioBuffer<float>& deviceBuffer, int numSamples, double sampleRate)
{
    if (auto* server = AudioServer::getInstanceWithoutCreating())
        server->pushSubscribedOutputs(clientId, deviceBuffer, numSamples, sampleRate);
}

void AudioClient::setSubscriptions(const AudioClientState& state)
{
    if (auto* server = AudioServer::getInstanceWithoutCreating())
        server->updateClientSubscriptions(clientId, state);
}

AudioClientState AudioClient::getSubscriptions() const
{
    if (auto* server = AudioServer::getInstanceWithoutCreating())
        return server->getClientState(clientId);
    return AudioClientState();
}

// ============================================================================
// AudioDeviceHandler implementation
// ============================================================================

AudioDeviceHandler::AudioDeviceHandler(const juce::String& name)
    : deviceName(name)
{
    deviceManager = std::make_unique<juce::AudioDeviceManager>();
}

AudioDeviceHandler::~AudioDeviceHandler()
{
    closeDevice();
}

bool AudioDeviceHandler::openDevice(const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup)
{
    bool wasAlreadyOpen = isDeviceOpen();

    if (wasAlreadyOpen)
    {
        DBG("AudioDeviceHandler: Device '" + deviceName + "' already open, ensuring callback is registered");
        // Ensure callback is registered (safe to call multiple times)
        deviceManager->addAudioCallback(this);
        return true;
    }

    // Add callback BEFORE opening device (important!)
    deviceManager->addAudioCallback(this);

    DBG("AudioDeviceHandler: Opening device '"
        + deviceName
        + "' in FULL-DUPLEX mode"
        + " sampleRate="
        + juce::String(preferredSetup.sampleRate)
        + " bufferSize="
        + juce::String(preferredSetup.bufferSize));

    // Step 1: Initialize device manager to make device types available
    deviceManager->initialiseWithDefaultDevices(0, 0);

    // Step 2: Find the device type
    juce::AudioIODeviceType* deviceType = nullptr;

    for (auto* type : deviceManager->getAvailableDeviceTypes())
    {
        auto inputDevices = type->getDeviceNames(true);
        auto outputDevices = type->getDeviceNames(false);

        // Check if this device type has our device
        if (inputDevices.contains(deviceName) || outputDevices.contains(deviceName))
        {
            deviceType = type;
            break;
        }
    }

    if (!deviceType)
    {
        DBG("AudioDeviceHandler: Failed to find device type for '" + deviceName + "'");
        deviceManager->removeAudioCallback(this);
        return false;
    }

    DBG("AudioDeviceHandler: Found device type: " + deviceType->getTypeName());

    // Step 3: Set the current device type in the manager
    deviceManager->setCurrentAudioDeviceType(deviceType->getTypeName(), true);

    // Step 4: Create setup
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.sampleRate = preferredSetup.sampleRate;
    setup.bufferSize = preferredSetup.bufferSize;

    // IMPORTANT: Device names may differ between input and output lists
    // Only set the device name where it actually appears
    auto inputDevices = deviceType->getDeviceNames(true);
    auto outputDevices = deviceType->getDeviceNames(false);
    bool deviceIsInput = inputDevices.contains(deviceName);
    bool deviceIsOutput = outputDevices.contains(deviceName);

    setup.inputDeviceName = deviceIsInput ? deviceName : juce::String();
    setup.outputDeviceName = deviceIsOutput ? deviceName : juce::String();

    DBG("AudioDeviceHandler: Device '"
        + deviceName
        + "' is "
        + (deviceIsInput ? "INPUT " : "")
        + (deviceIsOutput ? "OUTPUT" : ""));

    // IMPORTANT: Must explicitly enable channels for the device to start playing
    // JUCE won't start the device if no channels are enabled
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;

    // Copy channel configuration from preferredSetup - if they're zero, enable all
    setup.inputChannels = preferredSetup.inputChannels;
    setup.outputChannels = preferredSetup.outputChannels;

    if (setup.inputChannels.isZero() && setup.outputChannels.isZero())
    {
        // No specific channels - enable all available
        DBG("AudioDeviceHandler: No channels specified, enabling all");
        setup.inputChannels.setRange(0, 256, true);
        setup.outputChannels.setRange(0, 256, true); // JUCE will limit to actual count
    }
    else
    {
        DBG("AudioDeviceHandler: Using channel configuration from preferredSetup");
        DBG("  Input channels: " + setup.inputChannels.toString(2));
        DBG("  Output channels: " + setup.outputChannels.toString(2));
    }

    DBG("AudioDeviceHandler: Requested sampleRate="
        + juce::String(setup.sampleRate)
        + " (0=device default), bufferSize="
        + juce::String(setup.bufferSize)
        + " (0=device default)");

    // Step 5: Apply the setup
    juce::String error = deviceManager->setAudioDeviceSetup(setup, true);

    if (error.isEmpty())
    {
        auto* device = deviceManager->getCurrentAudioDevice();
        DBG("AudioDeviceHandler: Device opened successfully!");
        DBG("  Requested: '" + deviceName + "'");
        DBG("  Actual: '" + (device ? device->getName() : "NONE") + "'");
        DBG("  Type: " + (device ? device->getTypeName() : "NONE"));
        DBG("  Input channels: " + juce::String(device ? device->getActiveInputChannels().countNumberOfSetBits() : 0));
        DBG("  Output channels: "
            + juce::String(device ? device->getActiveOutputChannels().countNumberOfSetBits() : 0));
        DBG("  Buffer size: " + juce::String(device ? device->getCurrentBufferSizeSamples() : 0));
        DBG("  Sample rate: " + juce::String(device ? device->getCurrentSampleRate() : 0.0, 1));
        DBG("  Is playing: " + juce::String(device ? (device->isPlaying() ? "YES" : "NO") : "UNKNOWN"));

        // Populate cache with actual device channel info
        // This avoids needing to create temp devices later for channel queries
        if (device)
        {
            if (auto* server = atk::AudioServer::getInstanceWithoutCreating())
            {
                server->cacheDeviceInfo(
                    deviceName,
                    device->getInputChannelNames(),
                    device->getOutputChannelNames(),
                    device->getAvailableSampleRates(),
                    device->getAvailableBufferSizes()
                );
            }
        }

        // Register as change listener on AudioDeviceManager to detect device configuration changes
        deviceManager->addChangeListener(this);

        // Check if device actually started
        if (device && !device->isPlaying())
        {
            DBG("AudioDeviceHandler: WARNING - Device opened but not playing! Attempting to restart...");
            deviceManager->restartLastAudioDevice();
        }

        return true;
    }

    DBG("AudioDeviceHandler: Failed to open device '" + deviceName + "': " + error);
    deviceManager->removeAudioCallback(this);
    return false;
}

void AudioDeviceHandler::closeDevice()
{
    if (!isDeviceOpen())
        return;

    // Remove change listener before closing device
    deviceManager->removeChangeListener(this);

    deviceManager->removeAudioCallback(this);
    deviceManager->closeAudioDevice();
}

bool AudioDeviceHandler::isDeviceOpen() const
{
    return deviceManager->getCurrentAudioDevice() != nullptr;
}

void AudioDeviceHandler::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData,
    int numInputChannels,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& context
)
{
    // Clear output channels first - we'll accumulate into them
    if (outputChannelData)
        for (int ch = 0; ch < numOutputChannels; ++ch)
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

    // STEP 1: Process subscription-based routing (AudioClient mode)
    // Each subscriber gets clean input and writes to device output (summed)
    {
        auto snapshot = activeSnapshot.load(std::memory_order_acquire);

        if (isRunning.load(std::memory_order_acquire) && snapshot)
        {
            double deviceSampleRate = getSampleRate();

            // REAL-TIME SAFE: Process each client using lock-free snapshot
            for (const auto& [clientId, buffers] : snapshot->clients)
            {
                // Handle input: device -> client (subscribers get original clean input)
                if (inputChannelData && buffers.inputBuffer)
                {
                    // Prepare multichannel write buffer
                    int maxDevChannel = 0;
                    for (const auto& mapping : buffers.inputMappings)
                        maxDevChannel = std::max(maxDevChannel, mapping.deviceChannel.channelIndex);
                    int numDeviceChannels = std::min(maxDevChannel + 1, numInputChannels);

                    if (numDeviceChannels > 0
                        && numDeviceChannels <= static_cast<int>(rtInputPointers.size())
                        && numDeviceChannels <= numInputChannels)
                    {
                        // REAL-TIME SAFE: Use pre-allocated pointer array
                        for (int ch = 0; ch < numDeviceChannels; ++ch)
                            rtInputPointers[ch] = inputChannelData[ch];

                        // Write all device channels to multichannel SyncBuffer
                        buffers.inputBuffer
                            ->write(rtInputPointers.data(), numDeviceChannels, numSamples, deviceSampleRate);
                    }
                }

                // Handle output: client -> device (sum into output)
                if (outputChannelData && buffers.outputBuffer)
                {
                    // Determine how many device channels we need to read
                    int maxDevChannel = 0;
                    for (const auto& mapping : buffers.outputMappings)
                        maxDevChannel = std::max(maxDevChannel, mapping.deviceChannel.channelIndex);
                    int numDeviceChannels = std::min(maxDevChannel + 1, numOutputChannels);

                    if (numDeviceChannels > 0
                        && numDeviceChannels <= rtSubscriptionTempBuffer.getNumChannels()
                        && numDeviceChannels <= static_cast<int>(rtSubscriptionPointers.size()))
                    {
                        // REAL-TIME SAFE: Use pre-allocated buffer and pointer array
                        for (int ch = 0; ch < numDeviceChannels; ++ch)
                            rtSubscriptionPointers[ch] = rtSubscriptionTempBuffer.getWritePointer(ch);

                        if (buffers.outputBuffer->read(
                                rtSubscriptionPointers.data(),
                                numDeviceChannels,
                                numSamples,
                                deviceSampleRate,
                                false
                            ))
                        {
                            // Sum each channel into device output
                            for (int ch = 0; ch < numDeviceChannels; ++ch)
                                if (ch < numOutputChannels)
                                    juce::FloatVectorOperations::add(
                                        outputChannelData[ch],
                                        rtSubscriptionPointers[ch],
                                        numSamples
                                    );
                        }
                    }
                }
            }
        }
    }

    // STEP 2: Call direct callbacks if registered (PluginHost2 mode)
    // Each direct callback gets clean input and writes to its own temporary output buffer
    // All outputs are then summed into the final device output
    auto snapshot = directCallbackSnapshot.load(std::memory_order_acquire);
    if (snapshot != nullptr && !snapshot->callbacks.empty())
    {
        for (DirectCallbackInfo* info : snapshot->callbacks)
        {
            if (info == nullptr || info->callback == nullptr)
                continue;

            // Skip if temp buffer not sized correctly
            if (numOutputChannels > info->tempOutputBuffer.getNumChannels())
                continue;

            // Skip if output pointers vector not sized correctly (safety check)
            if (numOutputChannels > static_cast<int>(info->outputPointers.size()))
                continue;

            // REAL-TIME SAFE: Use pre-allocated buffer and pointer array
            for (int ch = 0; ch < numOutputChannels; ++ch)
                info->outputPointers[ch] = info->tempOutputBuffer.getWritePointer(ch);

            // Call the direct callback with clean input and its own temp output buffer
            info->callback->audioDeviceIOCallbackWithContext(
                inputChannelData, // Clean input (original device input)
                numInputChannels,
                info->outputPointers.data(), // Pre-allocated temp output buffer
                numOutputChannels,
                numSamples,
                context
            );

            // Sum this callback's output into the final device output
            for (int ch = 0; ch < numOutputChannels; ++ch)
            {
                juce::FloatVectorOperations::add(
                    outputChannelData[ch],
                    info->tempOutputBuffer.getReadPointer(ch),
                    numSamples
                );
            }
        }
    }
}

void AudioDeviceHandler::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    juce::ignoreUnused(device);
    DBG("AudioDeviceHandler: Device '" + deviceName + "' about to start - pre-allocating real-time buffers");

    // Pre-allocate buffers for real-time safe processing
    // Get maximum channel count and buffer size
    int maxChannels = std::max(
        device->getActiveInputChannels().countNumberOfSetBits(),
        device->getActiveOutputChannels().countNumberOfSetBits()
    );
    int bufferSize = device->getCurrentBufferSizeSamples();

    // Pre-allocate subscription processing buffers
    rtSubscriptionTempBuffer.setSize(maxChannels, bufferSize, false, false, true);
    rtSubscriptionPointers.resize(maxChannels);
    rtInputPointers.resize(maxChannels);

    // Pre-allocate direct callback buffers and notify callbacks
    {
        std::lock_guard<std::mutex> lock(directCallbackMutex);
        for (auto& [callback, info] : directCallbacks)
        {
            info.tempOutputBuffer.setSize(maxChannels, bufferSize, false, false, true);
            info.outputPointers.resize(maxChannels);

            // Notify the callback about the device starting
            if (callback != nullptr)
                callback->audioDeviceAboutToStart(device);
        }
        rebuildDirectCallbackSnapshotLocked();
    }

    // Check if we have active subscriptions and enable processing
    {
        std::lock_guard<std::mutex> lock(clientBuffersMutex);
        if (!clientBuffers.empty())
        {
            isRunning.store(true, std::memory_order_release);
            DBG("AudioDeviceHandler: Device '"
                + deviceName
                + "' ready for callbacks with "
                + juce::String(clientBuffers.size())
                + " active subscriptions (isRunning=true)");
        }
    }

    DBG("AudioDeviceHandler: Pre-allocated buffers: "
        + juce::String(maxChannels)
        + " channels, "
        + juce::String(bufferSize)
        + " samples");

    // NOTE: isRunning is also set in addClientSubscription when first subscription is added
    // This ensures both initial device open and parameter changes enable processing
}

void AudioDeviceHandler::audioDeviceStopped()
{
    DBG("AudioDeviceHandler: Device '" + deviceName + "' stopped");

    // Notify all direct callbacks that the device has stopped
    {
        std::lock_guard<std::mutex> lock(directCallbackMutex);
        for (auto& [callback, info] : directCallbacks)
            if (callback != nullptr)
                callback->audioDeviceStopped();
    }

    isRunning.store(false, std::memory_order_release);
}

void AudioDeviceHandler::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    juce::ignoreUnused(source);

    auto* device = deviceManager->getCurrentAudioDevice();
    if (!device)
        return;

    DBG("AudioDeviceHandler: Device '" + deviceName + "' configuration changed - updating cache");

    // Get new channel counts
    int newInputChannels = device->getActiveInputChannels().countNumberOfSetBits();
    int newOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();

    DBG("AudioDeviceHandler: New channel counts - "
        + juce::String(newInputChannels)
        + " inputs, "
        + juce::String(newOutputChannels)
        + " outputs");

    // Invalidate and update cache with new device info
    // Note: JUCE will call audioDeviceStopped() and audioDeviceAboutToStart()
    // when the device restarts, which will handle buffer reallocation
    if (auto* server = AudioServer::getInstanceWithoutCreating())
    {
        server->invalidateDeviceCache(deviceName);
        server->cacheDeviceInfo(
            deviceName,
            device->getInputChannelNames(),
            device->getOutputChannelNames(),
            device->getAvailableSampleRates(),
            device->getAvailableBufferSizes()
        );
    }
}

void AudioDeviceHandler::addClientSubscription(
    void* clientId,
    const std::vector<ChannelSubscription>& subscriptions,
    bool isInput
)
{
    // Convert subscriptions to mappings (clientChannel is just the subscription index)
    std::vector<ChannelMapping> mappings;
    for (size_t i = 0; i < subscriptions.size(); ++i)
    {
        ChannelMapping mapping;
        mapping.deviceChannel = subscriptions[i];
        mapping.clientChannel = static_cast<int>(i); // Index in subscription list
        mappings.push_back(mapping);
    }

    // Open device on first subscription (lazy initialization)
    // Do this BEFORE acquiring lock to avoid holding mutex during slow device open
    bool justOpened = false;
    if (!isDeviceOpen())
    {
        DBG("AudioDeviceHandler: Opening device '" + deviceName + "' on first subscription");

        // Use empty setup to let openDevice use device defaults
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.sampleRate = 0.0; // Signal to use device default
        setup.bufferSize = 0;   // Signal to use device default

        if (!openDevice(setup))
        {
            DBG("AudioDeviceHandler: ERROR - Failed to open device '" + deviceName + "' on first subscription");
            return;
        }

        justOpened = true;
        // Device is open but callbacks won't start yet (isRunning is still false)
    }

    // Now acquire lock and create SyncBuffers
    std::lock_guard<std::mutex> lock(clientBuffersMutex);

    auto& buffers = clientBuffers[clientId];

    if (isInput)
    {
        buffers.inputMappings = mappings;

        // Create single multichannel SyncBuffer for all device input channels
        if (!buffers.inputBuffer)
        {
            buffers.inputBuffer = std::make_shared<SyncBuffer>();

            // Use device's actual input channel count
            int numChannels = 2; // Default fallback
            if (auto* device = deviceManager->getCurrentAudioDevice())
                numChannels = device->getActiveInputChannels().countNumberOfSetBits();

            // Pre-configure reader side with OBS parameters (typical: 48kHz, 480 samples)
            // This allows writer (device callback) to prepare immediately
            juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
            dummyBuffer.clear();
            std::vector<float*> dummyPointers(numChannels);
            for (int ch = 0; ch < numChannels; ++ch)
                dummyPointers[ch] = dummyBuffer.getWritePointer(ch);

            buffers.inputBuffer->read(dummyPointers.data(), numChannels, 480, 48000.0, false);

            DBG("AudioDeviceHandler: Created multichannel input SyncBuffer for device '"
                + deviceName
                + "' with "
                + juce::String(numChannels)
                + " channels");
        }
    }
    else
    {
        buffers.outputMappings = mappings;

        // Create single multichannel SyncBuffer for all device output channels
        if (!buffers.outputBuffer)
        {
            buffers.outputBuffer = std::make_shared<SyncBuffer>();

            // Use device's actual output channel count
            int numChannels = 2; // Default fallback
            if (auto* device = deviceManager->getCurrentAudioDevice())
                numChannels = device->getActiveOutputChannels().countNumberOfSetBits();

            // Pre-configure writer side with OBS parameters (client will write at 48kHz, 480 samples)
            // This allows reader (device callback) to prepare immediately
            juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
            dummyBuffer.clear();
            std::vector<const float*> dummyPointers(numChannels);
            for (int ch = 0; ch < numChannels; ++ch)
                dummyPointers[ch] = dummyBuffer.getReadPointer(ch);

            buffers.outputBuffer->write(dummyPointers.data(), numChannels, 480, 48000.0);

            DBG("AudioDeviceHandler: Created multichannel output SyncBuffer for device '"
                + deviceName
                + "' with "
                + juce::String(numChannels)
                + " channels");
        }
    }

    // Rebuild snapshot so audio callback sees new buffers
    rebuildSnapshotLocked();

    // NOW it's safe to allow callbacks - all SyncBuffers are created
    if (justOpened)
    {
        isRunning.store(true, std::memory_order_release);
        DBG("AudioDeviceHandler: Device '" + deviceName + "' ready for callbacks (isRunning=true)");
    }
}

void AudioDeviceHandler::removeClientSubscription(void* clientId, bool isInput)
{
    bool shouldCloseDevice = false;

    {
        std::lock_guard<std::mutex> lock(clientBuffersMutex);

        auto it = clientBuffers.find(clientId);
        if (it != clientBuffers.end())
        {
            if (isInput)
            {
                it->second.inputBuffer.reset();
                it->second.inputMappings.clear();
            }
            else
            {
                it->second.outputBuffer.reset();
                it->second.outputMappings.clear();
            }

            // Remove client entry if no buffers left
            if (!it->second.inputBuffer && !it->second.outputBuffer)
                clientBuffers.erase(it);
        }

        // Rebuild snapshot to reflect changes
        rebuildSnapshotLocked();

        // Check if we should close device (no more subscriptions AND no direct callbacks)
        shouldCloseDevice = clientBuffers.empty() && !hasDirectCallback() && isDeviceOpen();
    }

    // Close device outside of lock if needed (only if no subscriptions AND no direct callbacks)
    if (shouldCloseDevice)
    {
        DBG("AudioDeviceHandler: Closing device '" + deviceName + "' - no more subscriptions");
        closeDevice();
    }
}

bool AudioDeviceHandler::hasActiveSubscriptions() const
{
    std::lock_guard<std::mutex> lock(clientBuffersMutex);
    return !clientBuffers.empty() || hasDirectCallback();
}

bool AudioDeviceHandler::registerDirectCallback(juce::AudioIODeviceCallback* callback)
{
    if (callback == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(directCallbackMutex);

    // Check if this callback is already registered
    if (directCallbacks.find(callback) != directCallbacks.end())
    {
        DBG("AudioDeviceHandler: Callback already registered for '" + deviceName + "'");
        return false;
    }

    // Create info with pre-allocated buffers
    DirectCallbackInfo info;
    info.callback = callback;

    // Pre-allocate buffers if device is already open
    auto* device = deviceManager->getCurrentAudioDevice();
    if (device != nullptr)
    {
        int maxChannels = std::max(
            device->getActiveInputChannels().countNumberOfSetBits(),
            device->getActiveOutputChannels().countNumberOfSetBits()
        );
        int bufferSize = device->getCurrentBufferSizeSamples();

        info.tempOutputBuffer.setSize(maxChannels, bufferSize, false, false, true);
        info.outputPointers.resize(maxChannels);

        // If device is already playing, notify the callback immediately
        // This is important for late-registered callbacks that join after device started
        if (device->isPlaying())
            callback->audioDeviceAboutToStart(device);
    }

    directCallbacks[callback] = std::move(info);
    rebuildDirectCallbackSnapshotLocked();

    return true;
}

void AudioDeviceHandler::unregisterDirectCallback(juce::AudioIODeviceCallback* callback)
{
    if (callback == nullptr)
        return;

    std::lock_guard<std::mutex> lock(directCallbackMutex);

    auto it = directCallbacks.find(callback);
    if (it != directCallbacks.end())
    {
        DBG("AudioDeviceHandler: Unregistering direct callback for '" + deviceName + "'");
        directCallbacks.erase(it);
        rebuildDirectCallbackSnapshotLocked();
    }
}

bool AudioDeviceHandler::hasDirectCallback() const
{
    std::lock_guard<std::mutex> lock(directCallbackMutex);
    return !directCallbacks.empty();
}

void AudioDeviceHandler::rebuildDirectCallbackSnapshotLocked()
{
    // Must be called while holding directCallbackMutex
    auto newSnapshot = std::make_shared<DirectCallbackSnapshot>();
    newSnapshot->callbacks.reserve(directCallbacks.size());

    for (auto& [callback, info] : directCallbacks)
        newSnapshot->callbacks.push_back(&info);

    // Atomic publish - audio callback can now see new snapshot
    directCallbackSnapshot.store(newSnapshot, std::memory_order_release);
}

int AudioDeviceHandler::getNumChannels() const
{
    if (auto* device = deviceManager->getCurrentAudioDevice())
    {
        // For full-duplex, return max of input and output channels
        int inputCh = device->getActiveInputChannels().countNumberOfSetBits();
        int outputCh = device->getActiveOutputChannels().countNumberOfSetBits();
        return std::max(inputCh, outputCh);
    }
    return 0;
}

double AudioDeviceHandler::getSampleRate() const
{
    if (auto* device = deviceManager->getCurrentAudioDevice())
        return device->getCurrentSampleRate();

    // Return 0.0 to indicate no device is open (caller should check)
    return 0.0;
}

int AudioDeviceHandler::getBufferSize() const
{
    if (auto* device = deviceManager->getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();

    // Return 0 to indicate no device is open (caller should check)
    return 0;
}

void AudioDeviceHandler::rebuildSnapshotLocked()
{
    // Must be called while holding clientBuffersMutex
    auto newSnapshot = std::make_shared<DeviceSnapshot>();
    newSnapshot->clients.reserve(clientBuffers.size());

    for (const auto& [clientId, buffers] : clientBuffers)
    {
        ClientBuffersSnapshot snapshot;
        snapshot.inputBuffer = buffers.inputBuffer;
        snapshot.outputBuffer = buffers.outputBuffer;
        snapshot.inputMappings = buffers.inputMappings;
        snapshot.outputMappings = buffers.outputMappings;

        newSnapshot->clients.emplace(clientId, std::move(snapshot));
    }

    // Atomic publish - audio callback can now see new snapshot
    activeSnapshot.store(newSnapshot, std::memory_order_release);
}

std::shared_ptr<AudioDeviceHandler::DeviceSnapshot> AudioDeviceHandler::getSnapshot() const
{
    return activeSnapshot.load(std::memory_order_acquire);
}

// ============================================================================
// AudioServer implementation
// ============================================================================

JUCE_IMPLEMENT_SINGLETON(AudioServer)

AudioServer::AudioServer()
{
}

AudioServer::~AudioServer()
{
    DBG("AudioServer: Destructor");
    shutdown();
    clearSingletonInstance();
}

juce::AudioDeviceManager* AudioServer::ensureDeviceEnumerator() const
{
    // Double-checked locking for thread-safe lazy initialization
    if (!deviceEnumerator)
    {
        std::lock_guard<std::mutex> lock(deviceEnumeratorMutex);
        if (!deviceEnumerator)
        {
            DBG("AudioServer: Lazy-initializing device enumerator");
            const_cast<AudioServer*>(this)->deviceEnumerator = std::make_unique<juce::AudioDeviceManager>();
        }
    }
    return deviceEnumerator.get();
}

void AudioServer::initialize()
{
    if (initialized.load(std::memory_order_acquire))
        return;

    DBG("AudioServer: Initializing...");

    // Note: Device enumerator will be created on-demand when UI queries devices
    // This avoids unnecessary device scanning at startup

    // Start timer for deferred device closing (10ms interval)
    startTimer(10);

    initialized.store(true, std::memory_order_release);

    DBG("AudioServer: Initialized");
}

void AudioServer::shutdown()
{
    if (!initialized.load(std::memory_order_acquire))
        return;

    initialized.store(false, std::memory_order_release);

    // Stop timer
    stopTimer();

    // Close all device handlers
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        pendingDeviceCloses.clear();
        deviceHandlers.clear();
    }

    // Clear all clients
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.clear();
    }

    // Clear device channel cache
    {
        std::lock_guard<std::mutex> lock(deviceChannelCacheMutex);
        inputDeviceChannelCache.clear();
        outputDeviceChannelCache.clear();
    }

    deviceEnumerator.reset();
}

void AudioServer::timerCallback()
{
    // Check for devices that should be closed
    std::vector<juce::String> devicesToClose;

    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        juce::int64 currentTime = juce::Time::currentTimeMillis();

        // Find devices whose close time has passed
        auto it = pendingDeviceCloses.begin();
        while (it != pendingDeviceCloses.end())
        {
            if (currentTime >= it->closeTime)
            {
                // Check if device still has no connections
                auto handlerIt = deviceHandlers.find(it->deviceName);
                if (handlerIt != deviceHandlers.end())
                {
                    if (!handlerIt->second->hasDirectCallback() && !handlerIt->second->hasActiveSubscriptions())
                        devicesToClose.push_back(it->deviceName);
                }
                it = pendingDeviceCloses.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Close devices
        for (const auto& deviceName : devicesToClose)
        {
            DBG("AudioServer: Closing device '" + deviceName + "' after deferred timeout");
            deviceHandlers.erase(deviceName);
        }
    }
}

void AudioServer::cancelPendingDeviceClose(const juce::String& deviceName)
{
    // Must be called with devicesMutex held
    auto it = std::find_if(
        pendingDeviceCloses.begin(),
        pendingDeviceCloses.end(),
        [&deviceName](const PendingDeviceClose& pending) { return pending.deviceName == deviceName; }
    );

    if (it != pendingDeviceCloses.end())
    {
        DBG("AudioServer: Cancelled pending close for device '" + deviceName + "'");
        pendingDeviceCloses.erase(it);
    }
}

void AudioServer::scheduleDeviceClose(const juce::String& deviceName)
{
    // Must be called with devicesMutex held
    // Schedule device to close in 5000ms (5 seconds)
    PendingDeviceClose pending;
    pending.deviceName = deviceName;
    pending.closeTime = juce::Time::currentTimeMillis() + 5000;

    pendingDeviceCloses.push_back(pending);
    DBG("AudioServer: Scheduled device '" + deviceName + "' for deferred close in 5 seconds");
}

void AudioServer::registerClient(void* clientId, const AudioClientState& state, int bufferSize)
{
    if (!initialized.load(std::memory_order_acquire))
        return;

    if (clientId == nullptr)
        return;

    {
        std::lock_guard<std::mutex> lock(clientsMutex);

        ClientInfo info;
        info.state.store(new AudioClientState(state), std::memory_order_release);
        info.bufferSize = bufferSize;

        clients[clientId] = std::move(info);
    }

    // Apply initial subscriptions (called outside clientsMutex to avoid lock ordering issues)
    updateClientSubscriptions(clientId, state);
}

void AudioServer::unregisterClient(void* clientId)
{
    if (!initialized.load(std::memory_order_acquire))
        return;

    if (clientId == nullptr)
        return;

    // Remove client from all device handlers
    {
        std::lock_guard<std::mutex> lock(devicesMutex);

        // First pass: remove subscriptions (don't erase handlers yet, would invalidate iterators)
        for (auto& [name, handler] : deviceHandlers)
        {
            handler->removeClientSubscription(clientId, true);  // Remove input subscriptions
            handler->removeClientSubscription(clientId, false); // Remove output subscriptions
        }

        // Second pass: schedule deferred close for unused handlers
        for (auto& [name, handler] : deviceHandlers)
            if (!handler->hasActiveSubscriptions() && !handler->hasDirectCallback())
                scheduleDeviceClose(name);
    }

    // Remove client info
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.erase(clientId);
    }
}

void AudioServer::updateClientSubscriptions(void* clientId, const AudioClientState& state)
{
    if (!initialized.load(std::memory_order_acquire))
        return;

    if (clientId == nullptr)
        return;

    // Check if subscriptions have actually changed
    bool subscriptionsChanged = false;
    {
        std::lock_guard<std::mutex> lock(clientsMutex);

        auto it = clients.find(clientId);
        if (it == clients.end())
            return;

        auto& clientInfo = it->second;

        // Load current state atomically
        auto* currentState = clientInfo.state.load(std::memory_order_acquire);

        // Compare with new state
        if (currentState && *currentState == state)
        {
            // Subscriptions haven't changed - skip update
            DBG("AudioServer: Subscriptions unchanged - skipping update ("
                << state.inputSubscriptions.size()
                << " in, "
                << state.outputSubscriptions.size()
                << " out)");
            return;
        }

        subscriptionsChanged = true;
    }

    // Subscriptions have changed - proceed with update
    DBG("AudioServer: Subscriptions changed - applying update ("
        << state.inputSubscriptions.size()
        << " in, "
        << state.outputSubscriptions.size()
        << " out)");

    // Update client state with atomic swap
    {
        std::lock_guard<std::mutex> lock(clientsMutex);

        auto it = clients.find(clientId);
        if (it == clients.end())
            return;

        auto& clientInfo = it->second;

        // Lock to serialize concurrent UI updates (UI thread only)
        const juce::ScopedLock sl(clientInfo.stateUpdateMutex);

        // Delete the PREVIOUS old state (guaranteed safe - audio thread has moved on)
        delete clientInfo.pendingDeleteState;

        // Create new state
        auto* newState = new AudioClientState(state);

        // Atomically swap the pointer (audio thread will see new state)
        clientInfo.pendingDeleteState = clientInfo.state.exchange(newState, std::memory_order_release);

        // DON'T delete immediately - defer until next update for 100% safety
    }

    // Update device handlers (acquire devicesMutex AFTER releasing clientsMutex to avoid deadlock)
    std::lock_guard<std::mutex> deviceLock(devicesMutex);

    // ATOMIC SWAP APPROACH: Build new subscription state, then swap atomically
    // This prevents race where device could be closed during remove->add window

    // Step 1: Group NEW subscriptions by device
    std::unordered_map<juce::String, std::vector<ChannelSubscription>> newInputSubs;
    std::unordered_map<juce::String, std::vector<ChannelSubscription>> newOutputSubs;

    for (const auto& sub : state.inputSubscriptions)
        newInputSubs[sub.deviceName].push_back(sub);

    for (const auto& sub : state.outputSubscriptions)
        newOutputSubs[sub.deviceName].push_back(sub);

    // Step 2: Get set of ALL devices (old + new)
    std::set<juce::String> allDevices;

    // Add devices from new state
    for (const auto& [deviceName, _] : newInputSubs)
        allDevices.insert(deviceName);
    for (const auto& [deviceName, _] : newOutputSubs)
        allDevices.insert(deviceName);

    // Add devices from old state (by checking existing handlers)
    // First, collect handler names while holding devicesMutex
    std::vector<juce::String> existingDeviceNames;
    {
        for (auto& [name, handler] : deviceHandlers)
            existingDeviceNames.push_back(name);
    }

    // Then check each handler's clientBuffers WITHOUT holding devicesMutex
    // to avoid nested locking devicesMutex -> clientBuffersMutex
    for (const auto& name : existingDeviceNames)
    {
        // Re-lookup handler (it might have been deleted)
        auto it = deviceHandlers.find(name);
        if (it != deviceHandlers.end())
        {
            auto* handler = it->second.get();
            std::lock_guard<std::mutex> handlerLock(handler->clientBuffersMutex);
            if (handler->clientBuffers.find(clientId) != handler->clientBuffers.end())
                allDevices.insert(name);
        }
    }

    // Step 3: For each device, atomically update subscriptions (remove old + add new in one operation)
    for (const auto& deviceName : allDevices)
    {
        // Cancel any pending close for this device (it's being reused)
        cancelPendingDeviceClose(deviceName);

        auto* handler = getOrCreateDeviceHandler(deviceName);
        if (!handler)
            continue;

        // Get new subscriptions for this device (empty if device no longer subscribed)
        auto newInputIt = newInputSubs.find(deviceName);
        auto newOutputIt = newOutputSubs.find(deviceName);

        std::vector<ChannelSubscription> newInput =
            (newInputIt != newInputSubs.end()) ? newInputIt->second : std::vector<ChannelSubscription>{};
        std::vector<ChannelSubscription> newOutput =
            (newOutputIt != newOutputSubs.end()) ? newOutputIt->second : std::vector<ChannelSubscription>{};

        // ATOMIC: Remove old + Add new in single locked operation
        {
            std::unique_lock<std::mutex> handlerLock(handler->clientBuffersMutex);
            bool snapshotDirty = false;

            auto bufferIt = handler->clientBuffers.find(clientId);
            if (bufferIt != handler->clientBuffers.end())
            {
                // Clear old subscriptions
                bufferIt->second.inputBuffer.reset();
                bufferIt->second.outputBuffer.reset();
                bufferIt->second.inputMappings.clear();
                bufferIt->second.outputMappings.clear();

                snapshotDirty = true;

                // If no new subscriptions for this device, remove client entry entirely
                if (newInput.empty() && newOutput.empty())
                {
                    handler->clientBuffers.erase(bufferIt);
                    handler->rebuildSnapshotLocked();
                    continue;
                }
            }
            else if (newInput.empty() && newOutput.empty())
            {
                // No old subscriptions, no new subscriptions - skip
                continue;
            }

            // Add new subscriptions (reuses addClientSubscription logic)
            // But we do it manually here to stay within the same lock
            if (!newInput.empty())
            {
                // Open device if needed (first subscription)
                bool justOpened = false;
                if (!handler->isDeviceOpen())
                {
                    DBG("AudioDeviceHandler: Opening device '" + deviceName + "' on subscription update");

                    // Use empty setup to let openDevice use device defaults
                    juce::AudioDeviceManager::AudioDeviceSetup setup;
                    setup.sampleRate = 0.0; // Signal to use device default
                    setup.bufferSize = 0;   // Signal to use device default

                    // Release handler lock during device open
                    handlerLock.unlock();
                    bool opened = handler->openDevice(setup);
                    handlerLock.lock();

                    if (!opened)
                    {
                        DBG("AudioDeviceHandler: ERROR - Failed to open device '" + deviceName + "'");
                        continue;
                    }

                    justOpened = true;
                }

                auto& buffers = handler->clientBuffers[clientId];

                // Convert subscriptions to mappings (clientChannel is subscription index)
                std::vector<ChannelMapping> inputMappings;
                for (size_t i = 0; i < newInput.size(); ++i)
                {
                    ChannelMapping mapping;
                    mapping.deviceChannel = newInput[i];
                    mapping.clientChannel = static_cast<int>(i);
                    inputMappings.push_back(mapping);
                }
                buffers.inputMappings = inputMappings;

                // Create single multichannel SyncBuffer if needed
                if (!buffers.inputBuffer)
                {
                    buffers.inputBuffer = std::make_shared<SyncBuffer>();

                    // Use device's actual input channel count
                    int numChannels = 2; // Default fallback
                    if (auto* device = handler->deviceManager->getCurrentAudioDevice())
                        numChannels = device->getActiveInputChannels().countNumberOfSetBits();

                    // Pre-configure reader side with OBS parameters (typical: 48kHz, 480 samples)
                    juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
                    dummyBuffer.clear();
                    std::vector<float*> dummyPointers(numChannels);
                    for (int ch = 0; ch < numChannels; ++ch)
                        dummyPointers[ch] = dummyBuffer.getWritePointer(ch);

                    buffers.inputBuffer->read(dummyPointers.data(), numChannels, 480, 48000.0, false);
                }

                snapshotDirty = true;

                // Enable subscription processing (even if device was already open by direct callback)
                if (justOpened || !handler->isRunning.load(std::memory_order_acquire))
                {
                    handler->isRunning.store(true, std::memory_order_release);
                    DBG("AudioDeviceHandler: Enabling subscription processing for device '" + deviceName + "'");
                }
            }

            if (!newOutput.empty())
            {
                // Open device if needed
                bool justOpened = false;
                if (!handler->isDeviceOpen())
                {
                    // Use empty setup to let openDevice use device defaults
                    juce::AudioDeviceManager::AudioDeviceSetup setup;
                    setup.sampleRate = 0.0; // Signal to use device default
                    setup.bufferSize = 0;   // Signal to use device default

                    handlerLock.unlock();
                    bool opened = handler->openDevice(setup);
                    handlerLock.lock();

                    if (!opened)
                        continue;

                    justOpened = true;
                }

                auto& buffers = handler->clientBuffers[clientId];

                // Convert subscriptions to mappings (clientChannel is subscription index)
                std::vector<ChannelMapping> outputMappings;
                for (size_t i = 0; i < newOutput.size(); ++i)
                {
                    ChannelMapping mapping;
                    mapping.deviceChannel = newOutput[i];
                    mapping.clientChannel = static_cast<int>(i);
                    outputMappings.push_back(mapping);
                }
                buffers.outputMappings = outputMappings;

                // Create single multichannel SyncBuffer if needed
                if (!buffers.outputBuffer)
                {
                    buffers.outputBuffer = std::make_shared<SyncBuffer>();

                    // Use device's actual output channel count
                    int numChannels = 2; // Default fallback
                    if (auto* device = handler->deviceManager->getCurrentAudioDevice())
                        numChannels = device->getActiveOutputChannels().countNumberOfSetBits();

                    // Pre-configure writer side with OBS parameters (client will write at 48kHz, 480 samples)
                    juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
                    dummyBuffer.clear();
                    std::vector<const float*> dummyPointers(numChannels);
                    for (int ch = 0; ch < numChannels; ++ch)
                        dummyPointers[ch] = dummyBuffer.getReadPointer(ch);

                    buffers.outputBuffer->write(dummyPointers.data(), numChannels, 480, 48000.0);
                }

                snapshotDirty = true;

                // Enable subscription processing (even if device was already open by direct callback)
                if (justOpened || !handler->isRunning.load(std::memory_order_acquire))
                {
                    handler->isRunning.store(true, std::memory_order_release);
                    DBG("AudioDeviceHandler: Enabling subscription processing for device '" + deviceName + "'");
                }
            }

            // Rebuild snapshot if anything changed
            if (snapshotDirty)
                handler->rebuildSnapshotLocked();
        }

        // Close device if no more subscriptions (outside handler lock to avoid deadlock)
        if (!handler->hasActiveSubscriptions() && handler->isDeviceOpen())
        {
            DBG("AudioServer: Closing device '" + deviceName + "' - no more subscriptions after update");
            handler->closeDevice();
        }
    }

    // Step 4: Clean up unused device handlers
    std::vector<juce::String> handlersToRemove;
    for (auto& [name, handler] : deviceHandlers)
        if (!handler->hasActiveSubscriptions())
            handlersToRemove.push_back(name);

    for (const auto& name : handlersToRemove)
    {
        DBG("AudioServer: Removing unused device handler '" + name + "'");
        deviceHandlers.erase(name);
    }

    DBG("AudioServer: Updated subscriptions for client "
        + juce::String::toHexString((juce::pointer_sized_int)clientId));
}

AudioClientState AudioServer::getClientState(void* clientId) const
{
    if (!initialized.load(std::memory_order_acquire))
        return AudioClientState();

    if (clientId == nullptr)
        return AudioClientState();

    std::lock_guard<std::mutex> lock(clientsMutex);

    auto it = clients.find(clientId);
    if (it != clients.end())
    {
        auto* statePtr = it->second.state.load(std::memory_order_acquire);
        return statePtr ? *statePtr : AudioClientState();
    }

    return AudioClientState();
}

void AudioServer::pullSubscribedInputs(
    void* clientId,
    juce::AudioBuffer<float>& deviceBuffer,
    int numSamples,
    double clientSampleRate
)
{
    if (!initialized.load(std::memory_order_acquire))
        return;

    if (clientId == nullptr)
        return;

    // Get client's input subscriptions with lock-free atomic load
    AudioClientState state;
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        auto it = clients.find(clientId);
        if (it != clients.end())
        {
            auto* statePtr = it->second.state.load(std::memory_order_acquire);
            if (statePtr)
                state = *statePtr;
            else
                return;
        }
        else
            return;
    }

    // Ensure deviceBuffer has enough channels (one per subscription)
    int numSubscriptions = (int)state.inputSubscriptions.size();
    if (deviceBuffer.getNumChannels() < numSubscriptions)
        return; // Buffer too small

    // Clear device buffer
    deviceBuffer.clear();

    // Group subscriptions by device
    std::unordered_map<juce::String, std::vector<std::pair<int, ChannelSubscription>>> deviceSubs;
    for (int i = 0; i < numSubscriptions; ++i)
        deviceSubs[state.inputSubscriptions[i].deviceName].push_back({i, state.inputSubscriptions[i]});

    std::lock_guard<std::mutex> deviceLock(devicesMutex);

    for (const auto& [deviceName, subs] : deviceSubs)
    {
        auto it = deviceHandlers.find(deviceName);
        if (it != deviceHandlers.end())
        {
            auto* handler = it->second.get();

            // Lock to safely access clientBuffers map
            std::lock_guard<std::mutex> bufferLock(handler->clientBuffersMutex);
            auto clientIt = handler->clientBuffers.find(clientId);
            if (clientIt != handler->clientBuffers.end())
            {
                auto& buffers = clientIt->second;

                // Read multichannel audio from SyncBuffer
                if (buffers.inputBuffer)
                {
                    // Determine number of device channels we need
                    int maxDevChannel = 0;
                    for (const auto& [subIdx, sub] : subs)
                        maxDevChannel = std::max(maxDevChannel, sub.channelIndex);
                    int numDeviceChannels = maxDevChannel + 1;

                    // Read from multichannel SyncBuffer
                    juce::AudioBuffer<float> tempBuffer(numDeviceChannels, numSamples);
                    std::vector<float*> devicePointers(numDeviceChannels);
                    for (int ch = 0; ch < numDeviceChannels; ++ch)
                        devicePointers[ch] = tempBuffer.getWritePointer(ch);

                    bool success =
                        buffers.inputBuffer
                            ->read(devicePointers.data(), numDeviceChannels, numSamples, clientSampleRate, false);

                    if (success)
                    {
                        // Copy device channels to deviceBuffer
                        for (const auto& [subIdx, sub] : subs)
                        {
                            int devCh = sub.channelIndex;
                            if (devCh < numDeviceChannels && subIdx < deviceBuffer.getNumChannels())
                                deviceBuffer.copyFrom(subIdx, 0, tempBuffer, devCh, 0, numSamples);
                        }
                    }
                }
            }
        }
    }
}

void AudioServer::pushSubscribedOutputs(
    void* clientId,
    const juce::AudioBuffer<float>& deviceBuffer,
    int numSamples,
    double clientSampleRate
)
{
    if (!initialized.load(std::memory_order_acquire))
        return;

    if (clientId == nullptr)
        return;

    // Get client's output subscriptions with lock-free atomic load
    AudioClientState state;
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        auto it = clients.find(clientId);
        if (it != clients.end())
        {
            auto* statePtr = it->second.state.load(std::memory_order_acquire);
            if (statePtr)
                state = *statePtr;
            else
                return;
        }
        else
            return;
    }

    int numSubscriptions = (int)state.outputSubscriptions.size();
    if (deviceBuffer.getNumChannels() < numSubscriptions)
        return; // Buffer too small

    // Group subscriptions by device
    std::unordered_map<juce::String, std::vector<std::pair<int, ChannelSubscription>>> deviceSubs;
    for (int i = 0; i < numSubscriptions; ++i)
        deviceSubs[state.outputSubscriptions[i].deviceName].push_back({i, state.outputSubscriptions[i]});

    std::lock_guard<std::mutex> deviceLock(devicesMutex);

    for (const auto& [deviceName, subs] : deviceSubs)
    {
        auto it = deviceHandlers.find(deviceName);
        if (it != deviceHandlers.end())
        {
            auto* handler = it->second.get();

            // Lock to safely access clientBuffers map
            std::lock_guard<std::mutex> bufferLock(handler->clientBuffersMutex);
            auto clientIt = handler->clientBuffers.find(clientId);
            if (clientIt != handler->clientBuffers.end())
            {
                auto& buffers = clientIt->second;

                // Write multichannel audio to SyncBuffer
                if (buffers.outputBuffer)
                {
                    // Determine number of device channels we need
                    int maxDevChannel = 0;
                    for (const auto& [subIdx, sub] : subs)
                        maxDevChannel = std::max(maxDevChannel, sub.channelIndex);
                    int numDeviceChannels = maxDevChannel + 1;

                    // Build multichannel buffer from subscription channels
                    juce::AudioBuffer<float> tempBuffer(numDeviceChannels, numSamples);
                    tempBuffer.clear();

                    // Copy subscription channels to device channels
                    for (const auto& [subIdx, sub] : subs)
                    {
                        int devCh = sub.channelIndex;
                        if (subIdx < deviceBuffer.getNumChannels() && devCh < numDeviceChannels)
                            tempBuffer.copyFrom(devCh, 0, deviceBuffer, subIdx, 0, numSamples);
                    }

                    // Write all device channels to multichannel SyncBuffer
                    std::vector<const float*> devicePointers(numDeviceChannels);
                    for (int ch = 0; ch < numDeviceChannels; ++ch)
                        devicePointers[ch] = tempBuffer.getReadPointer(ch);

                    buffers.outputBuffer->write(devicePointers.data(), numDeviceChannels, numSamples, clientSampleRate);
                }
            }
        }
    }
}

juce::StringArray AudioServer::getAvailableInputDevices() const
{
    // Lazily initialize device enumerator with thread safety
    auto* enumerator = ensureDeviceEnumerator();
    if (!enumerator)
        return juce::StringArray();

    juce::StringArray devices;
    for (auto& type : enumerator->getAvailableDeviceTypes())
    {
        type->scanForDevices();
        devices.addArray(type->getDeviceNames(true));
    }

    devices.removeDuplicates(false);
    return devices;
}

juce::StringArray AudioServer::getAvailableOutputDevices() const
{
    // Lazily initialize device enumerator with thread safety
    auto* enumerator = ensureDeviceEnumerator();
    if (!enumerator)
        return juce::StringArray();

    juce::StringArray devices;
    for (auto& type : enumerator->getAvailableDeviceTypes())
    {
        type->scanForDevices();
        devices.addArray(type->getDeviceNames(false));
    }

    devices.removeDuplicates(false);
    return devices;
}

std::map<juce::String, juce::StringArray> AudioServer::getInputDevicesByType() const
{
    // Lazily initialize device enumerator with thread safety
    auto* enumerator = ensureDeviceEnumerator();
    if (!enumerator)
        return std::map<juce::String, juce::StringArray>();

    std::map<juce::String, juce::StringArray> devicesByType;
    for (auto& type : enumerator->getAvailableDeviceTypes())
    {
        type->scanForDevices();
        auto devices = type->getDeviceNames(true);
        if (devices.size() > 0)
            devicesByType[type->getTypeName()] = devices;
    }

    return devicesByType;
}

std::map<juce::String, juce::StringArray> AudioServer::getOutputDevicesByType() const
{
    // Lazily initialize device enumerator with thread safety
    auto* enumerator = ensureDeviceEnumerator();
    if (!enumerator)
        return std::map<juce::String, juce::StringArray>();

    std::map<juce::String, juce::StringArray> devicesByType;
    for (auto& type : enumerator->getAvailableDeviceTypes())
    {
        type->scanForDevices();
        auto devices = type->getDeviceNames(false);
        if (devices.size() > 0)
            devicesByType[type->getTypeName()] = devices;
    }

    return devicesByType;
}

int AudioServer::getDeviceNumChannels(const juce::String& deviceName, bool isInput) const
{
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(deviceChannelCacheMutex);
        const auto& cache = isInput ? inputDeviceChannelCache : outputDeviceChannelCache;
        auto it = cache.find(deviceName);
        if (it != cache.end())
            return it->second;
    }

    // IMPORTANT: Check if device is already open - query from active device to avoid conflicts
    {
        auto it = deviceHandlers.find(deviceName);
        if (it != deviceHandlers.end() && it->second->isDeviceOpen())
        {
            auto* device = it->second->deviceManager->getCurrentAudioDevice();
            if (device)
            {
                // Since we have the device open, cache BOTH input and output channel info
                int numInputChannels = device->getActiveInputChannels().countNumberOfSetBits();
                int numOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();

                juce::StringArray inputChannelNames = device->getInputChannelNames();
                juce::StringArray outputChannelNames = device->getOutputChannelNames();

                // Cache both input and output info while we have the device open
                {
                    std::lock_guard<std::mutex> lock(deviceChannelCacheMutex);
                    auto* server = const_cast<AudioServer*>(this);

                    server->inputDeviceChannelCache[deviceName] = numInputChannels;
                    server->inputDeviceChannelNamesCache[deviceName] = inputChannelNames;

                    server->outputDeviceChannelCache[deviceName] = numOutputChannels;
                    server->outputDeviceChannelNamesCache[deviceName] = outputChannelNames;
                }

                return isInput ? numInputChannels : numOutputChannels;
            }
        }
    }

    // Lazily initialize device enumerator with thread safety (only for devices that aren't already open)
    auto* enumerator = ensureDeviceEnumerator();
    if (!enumerator)
        return 0; // Return 0 if initialization failed

    // Query channel count from device (requires temporary device opening)
    // NOTE: We create the temp device but DON'T open it - just query its capabilities
    int numChannels = 0;
    juce::StringArray channelNames;
    for (auto& type : enumerator->getAvailableDeviceTypes())
    {
        type->scanForDevices();

        // Check BOTH input and output device lists to find the device
        auto inputDevices = type->getDeviceNames(true);
        auto outputDevices = type->getDeviceNames(false);

        bool foundInInputs = inputDevices.contains(deviceName);
        bool foundInOutputs = outputDevices.contains(deviceName);

        if (foundInInputs || foundInOutputs)
        {
            // Create device in unique_ptr to ensure proper cleanup
            // The device is created but NOT opened - we just query channel names
            std::unique_ptr<juce::AudioIODevice> device(type->createDevice(deviceName, deviceName));
            if (device)
            {
                // Get channel names WITHOUT opening the device
                juce::StringArray inputChannelNames = device->getInputChannelNames();
                juce::StringArray outputChannelNames = device->getOutputChannelNames();

                // Return the requested direction
                if (isInput)
                {
                    channelNames = inputChannelNames;
                    numChannels = channelNames.size();
                }
                else
                {
                    channelNames = outputChannelNames;
                    numChannels = channelNames.size();
                }

                // Cache both input and output info
                {
                    std::lock_guard<std::mutex> lock(deviceChannelCacheMutex);
                    auto* server = const_cast<AudioServer*>(this);

                    server->inputDeviceChannelCache[deviceName] = inputChannelNames.size();
                    server->inputDeviceChannelNamesCache[deviceName] = inputChannelNames;

                    server->outputDeviceChannelCache[deviceName] = outputChannelNames.size();
                    server->outputDeviceChannelNamesCache[deviceName] = outputChannelNames;
                }

                // Device is automatically closed/destroyed when unique_ptr goes out of scope
                break;
            }
        }
    }

    return numChannels;
}

juce::StringArray AudioServer::getDeviceChannelNames(const juce::String& deviceName, bool isInput) const
{
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(deviceChannelCacheMutex);
        const auto& cache = isInput ? inputDeviceChannelNamesCache : outputDeviceChannelNamesCache;
        auto it = cache.find(deviceName);
        if (it != cache.end())
            return it->second;
    }

    // If not cached, call getDeviceNumChannels which will populate both caches
    getDeviceNumChannels(deviceName, isInput);

    // Now retrieve from cache
    {
        std::lock_guard<std::mutex> lock(deviceChannelCacheMutex);
        const auto& cache = isInput ? inputDeviceChannelNamesCache : outputDeviceChannelNamesCache;
        auto it = cache.find(deviceName);
        if (it != cache.end())
            return it->second;
    }

    // If still not cached, return empty array
    return juce::StringArray();
}

juce::Array<double> AudioServer::getAvailableSampleRates(const juce::String& deviceName) const
{
    // Check cache first (works whether device is open or not)
    {
        std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
        auto it = deviceSampleRatesCache.find(deviceName);
        if (it != deviceSampleRatesCache.end())
            return it->second;
    }

    // Not in cache - query device capabilities
    juce::Array<double> rates;

    // Lazily initialize device enumerator with thread safety
    auto* enumerator = ensureDeviceEnumerator();
    if (!enumerator)
        return rates; // Return empty array if initialization failed

    // Find the device type that has this device
    for (int i = 0; i < enumerator->getAvailableDeviceTypes().size(); ++i)
    {
        auto* type = enumerator->getAvailableDeviceTypes()[i];
        if (!type)
            continue;

        auto outputDevices = type->getDeviceNames(false);
        if (outputDevices.contains(deviceName))
        {
            // Create device but DON'T open it - just query capabilities
            std::unique_ptr<juce::AudioIODevice> device(type->createDevice(deviceName, deviceName));
            if (device)
            {
                rates = device->getAvailableSampleRates();
                // Device is automatically destroyed when unique_ptr goes out of scope
                break;
            }
        }
    }

    // Cache the result
    {
        std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
        deviceSampleRatesCache[deviceName] = rates;
    }

    return rates;
}

juce::Array<int> AudioServer::getAvailableBufferSizes(const juce::String& deviceName) const
{
    // Check cache first (works whether device is open or not)
    {
        std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
        auto it = deviceBufferSizesCache.find(deviceName);
        if (it != deviceBufferSizesCache.end())
            return it->second;
    }

    // Not in cache - query device capabilities
    juce::Array<int> sizes;

    // Lazily initialize device enumerator with thread safety
    auto* enumerator = ensureDeviceEnumerator();
    if (!enumerator)
        return sizes; // Return empty array if initialization failed

    // Find the device type that has this device
    for (int i = 0; i < enumerator->getAvailableDeviceTypes().size(); ++i)
    {
        auto* type = enumerator->getAvailableDeviceTypes()[i];
        if (!type)
            continue;

        auto outputDevices = type->getDeviceNames(false);
        if (outputDevices.contains(deviceName))
        {
            // Create device but DON'T open it - just query capabilities
            std::unique_ptr<juce::AudioIODevice> device(type->createDevice(deviceName, deviceName));
            if (device)
            {
                sizes = device->getAvailableBufferSizes();
                // Device is automatically destroyed when unique_ptr goes out of scope
                break;
            }
        }
    }

    // Cache the result
    {
        std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
        deviceBufferSizesCache[deviceName] = sizes;
    }

    return sizes;
}

void AudioServer::cacheDeviceInfo(
    const juce::String& deviceName,
    const juce::StringArray& inputChannelNames,
    const juce::StringArray& outputChannelNames,
    const juce::Array<double>& sampleRates,
    const juce::Array<int>& bufferSizes
)
{
    // Cache channel counts and names
    {
        std::lock_guard<std::mutex> lock(deviceChannelCacheMutex);
        inputDeviceChannelCache[deviceName] = inputChannelNames.size();
        outputDeviceChannelCache[deviceName] = outputChannelNames.size();
        inputDeviceChannelNamesCache[deviceName] = inputChannelNames;
        outputDeviceChannelNamesCache[deviceName] = outputChannelNames;
    }

    // Cache capabilities
    {
        std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
        deviceSampleRatesCache[deviceName] = sampleRates;
        deviceBufferSizesCache[deviceName] = bufferSizes;
    }

    DBG("AudioServer: Cached device info for '"
        + deviceName
        + "' - "
        + juce::String(inputChannelNames.size())
        + " inputs, "
        + juce::String(outputChannelNames.size())
        + " outputs");
}

void AudioServer::invalidateDeviceCache(const juce::String& deviceName)
{
    DBG("AudioServer: Invalidating device cache for '" + deviceName + "'");

    // Clear channel counts and names
    {
        std::lock_guard<std::mutex> lock(deviceChannelCacheMutex);
        inputDeviceChannelCache.erase(deviceName);
        outputDeviceChannelCache.erase(deviceName);
        inputDeviceChannelNamesCache.erase(deviceName);
        outputDeviceChannelNamesCache.erase(deviceName);
    }

    // Clear capabilities cache
    {
        std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
        deviceSampleRatesCache.erase(deviceName);
        deviceBufferSizesCache.erase(deviceName);
    }
}

double AudioServer::getCurrentSampleRate(const juce::String& deviceName) const
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    auto it = deviceHandlers.find(deviceName);
    if (it != deviceHandlers.end() && it->second->isDeviceOpen())
        return it->second->getSampleRate();

    return 0.0;
}

int AudioServer::getCurrentBufferSize(const juce::String& deviceName) const
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    auto it = deviceHandlers.find(deviceName);
    if (it != deviceHandlers.end() && it->second->isDeviceOpen())
        return it->second->getBufferSize();

    return 0;
}

AudioDeviceHandler* AudioServer::getOrCreateDeviceHandler(const juce::String& deviceName)
{
    auto it = deviceHandlers.find(deviceName);
    if (it != deviceHandlers.end())
    {
        DBG("AudioServer: Reusing existing device handler for '" + deviceName + "'");
        return it->second.get();
    }

    DBG("AudioServer: Creating NEW device handler for '" + deviceName + "'");

    // Create new handler (does NOT open the device yet - will open on first subscription)
    auto handler = std::make_unique<AudioDeviceHandler>(deviceName);
    auto* ptr = handler.get();
    deviceHandlers[deviceName] = std::move(handler);

    return ptr;
}

void AudioServer::removeDeviceHandlerIfUnused(const juce::String& deviceName)
{
    auto it = deviceHandlers.find(deviceName);
    if (it != deviceHandlers.end() && !it->second->hasActiveSubscriptions())
    {
        DBG("AudioServer: Closing unused device '" + deviceName + "'");
        deviceHandlers.erase(it);
    }
}

bool AudioServer::registerDirectCallback(
    const juce::String& deviceName,
    juce::AudioIODeviceCallback* callback,
    const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup
)
{
    if (!initialized.load(std::memory_order_acquire))
    {
        DBG("AudioServer: Cannot register direct callback - server not initialized");
        return false;
    }

    if (callback == nullptr)
    {
        DBG("AudioServer: Cannot register null callback");
        return false;
    }

    std::lock_guard<std::mutex> lock(devicesMutex);

    // Get or create device handler
    auto* handler = getOrCreateDeviceHandler(deviceName);
    if (handler == nullptr)
    {
        DBG("AudioServer: Failed to create device handler for '" + deviceName + "'");
        return false;
    }

    // Cancel any pending close for this device (it's being reused)
    cancelPendingDeviceClose(deviceName);

    // Try to register the direct callback
    if (!handler->registerDirectCallback(callback))
        return false;

    // Open device if not already open
    // Check if device needs to be reopened due to parameter changes
    bool needsReopen = false;
    if (handler->isDeviceOpen())
    {
        if (auto* device = handler->deviceManager->getCurrentAudioDevice())
        {
            double currentRate = device->getCurrentSampleRate();
            int currentBuffer = device->getCurrentBufferSizeSamples();

            // If user specified explicit parameters (non-zero), check if they differ
            if (preferredSetup.sampleRate > 0.0 && !juce::exactlyEqual(currentRate, preferredSetup.sampleRate))
                needsReopen = true;
            if (preferredSetup.bufferSize > 0 && currentBuffer != preferredSetup.bufferSize)
                needsReopen = true;
        }
    }

    if (needsReopen)
        handler->closeDevice();

    if (!handler->isDeviceOpen())
    {
        if (!handler->openDevice(preferredSetup))
        {
            handler->unregisterDirectCallback(callback);
            DBG("AudioServer: Failed to open device '" + deviceName + "' for direct callback");
            return false;
        }
    }
    else if (auto* device = handler->deviceManager->getCurrentAudioDevice())
    {
        // Device already open - ensure it's playing
        if (!device->isPlaying())
            handler->deviceManager->restartLastAudioDevice();
    }

    DBG("AudioServer: Successfully registered direct callback for device '" + deviceName + "'");
    return true;
}

void AudioServer::unregisterDirectCallback(const juce::String& deviceName, juce::AudioIODeviceCallback* callback)
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    auto it = deviceHandlers.find(deviceName);
    if (it != deviceHandlers.end())
    {
        it->second->unregisterDirectCallback(callback);

        // Schedule deferred close if no longer needed
        if (!it->second->hasDirectCallback() && !it->second->hasActiveSubscriptions())
            scheduleDeviceClose(deviceName);
    }
}

bool AudioServer::hasDirectCallback(const juce::String& deviceName) const
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    auto it = deviceHandlers.find(deviceName);
    if (it != deviceHandlers.end())
        return it->second->hasDirectCallback();

    return false;
}

bool AudioServer::setDeviceSampleRate(const juce::String& deviceName, double newSampleRate)
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    auto it = deviceHandlers.find(deviceName);
    if (it == deviceHandlers.end())
    {
        DBG("AudioServer::setDeviceSampleRate - Device '" + deviceName + "' not found");
        return false;
    }

    auto& handler = it->second;
    if (!handler->isDeviceOpen())
    {
        DBG("AudioServer::setDeviceSampleRate - Device '" + deviceName + "' is not open");
        return false;
    }

    auto* device = handler->deviceManager->getCurrentAudioDevice();
    if (!device)
    {
        DBG("AudioServer::setDeviceSampleRate - No active device for '" + deviceName + "'");
        return false;
    }

    // Check if the rate is supported
    auto availableRates = device->getAvailableSampleRates();
    if (!availableRates.contains(newSampleRate))
    {
        DBG("AudioServer::setDeviceSampleRate - Sample rate "
            + juce::String(newSampleRate)
            + " Hz not supported by device '"
            + deviceName
            + "'");
        return false;
    }

    DBG("AudioServer::setDeviceSampleRate - Changing '"
        + deviceName
        + "' from "
        + juce::String(device->getCurrentSampleRate())
        + " Hz to "
        + juce::String(newSampleRate)
        + " Hz");

    // Get current setup to preserve device names
    auto currentSetup = handler->deviceManager->getAudioDeviceSetup();

    // Create new setup with changed sample rate, keep everything else
    juce::AudioDeviceManager::AudioDeviceSetup newSetup = currentSetup;
    newSetup.sampleRate = newSampleRate;
    // Keep all channels enabled
    newSetup.inputChannels.setRange(0, 256, true);
    newSetup.outputChannels.setRange(0, 256, true);

    // Use device manager to properly handle callback stop/start
    juce::String error = handler->deviceManager->setAudioDeviceSetup(newSetup, true);

    if (error.isNotEmpty())
    {
        DBG("AudioServer::setDeviceSampleRate - Failed: " + error);
        return false;
    }

    DBG("AudioServer::setDeviceSampleRate - Successfully changed sample rate");
    return true;
}

bool AudioServer::setDeviceBufferSize(const juce::String& deviceName, int newBufferSize)
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    auto it = deviceHandlers.find(deviceName);
    if (it == deviceHandlers.end())
    {
        DBG("AudioServer::setDeviceBufferSize - Device '" + deviceName + "' not found");
        return false;
    }

    auto& handler = it->second;
    if (!handler->isDeviceOpen())
    {
        DBG("AudioServer::setDeviceBufferSize - Device '" + deviceName + "' is not open");
        return false;
    }

    auto* device = handler->deviceManager->getCurrentAudioDevice();
    if (!device)
    {
        DBG("AudioServer::setDeviceBufferSize - No active device for '" + deviceName + "'");
        return false;
    }

    // Check if the buffer size is supported
    auto availableSizes = device->getAvailableBufferSizes();
    if (!availableSizes.contains(newBufferSize))
    {
        DBG("AudioServer::setDeviceBufferSize - Buffer size "
            + juce::String(newBufferSize)
            + " samples not supported by device '"
            + deviceName
            + "'");
        return false;
    }

    DBG("AudioServer::setDeviceBufferSize - Changing '"
        + deviceName
        + "' from "
        + juce::String(device->getCurrentBufferSizeSamples())
        + " to "
        + juce::String(newBufferSize)
        + " samples");

    // Get current setup to preserve device names
    auto currentSetup = handler->deviceManager->getAudioDeviceSetup();

    // Create new setup with changed buffer size, keep everything else
    juce::AudioDeviceManager::AudioDeviceSetup newSetup = currentSetup;
    newSetup.bufferSize = newBufferSize;
    // Keep all channels enabled
    newSetup.inputChannels.setRange(0, 256, true);
    newSetup.outputChannels.setRange(0, 256, true);

    // Use device manager to properly handle callback stop/start
    juce::String error = handler->deviceManager->setAudioDeviceSetup(newSetup, true);

    if (error.isNotEmpty())
    {
        DBG("AudioServer::setDeviceBufferSize - Failed: " + error);
        return false;
    }

    DBG("AudioServer::setDeviceBufferSize - Successfully changed buffer size");
    return true;
}

bool AudioServer::getCurrentDeviceSetup(
    const juce::String& deviceName,
    juce::AudioDeviceManager::AudioDeviceSetup& outSetup
) const
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    auto it = deviceHandlers.find(deviceName);
    if (it == deviceHandlers.end() || !it->second)
        return false;

    auto* handler = it->second.get();
    if (!handler->isDeviceOpen())
        return false;

    outSetup = handler->deviceManager->getAudioDeviceSetup();
    return true;
}

AudioDeviceHandler* AudioServer::getDeviceHandler(const juce::String& deviceName) const
{
    std::lock_guard<std::mutex> lock(devicesMutex);
    auto it = deviceHandlers.find(deviceName);
    if (it != deviceHandlers.end())
        return it->second.get();
    return nullptr;
}

} // namespace atk
