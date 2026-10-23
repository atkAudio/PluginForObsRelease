#include "AudioServer.h"
#include <atkaudio/atkaudio.h>
#include <atkaudio/GlobalSettings.h>
#include <atkaudio/Logging.h>
#include <thread>

namespace atk
{

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

// AudioClient

AudioClient::AudioClient(int bufferSize)
    : clientId(this)
    , clientBufferSize(bufferSize)
{
    // Pre-allocate temp buffers with reasonable initial size
    tempInputBuffer.setSize(8, 1024, false, false, true);
    tempOutputBuffer.setSize(8, 1024, false, false, true);
    tempInputPointers.resize(8);
    tempOutputPointers.resize(8);

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
    , clientBufferSize(other.clientBufferSize)
    , bufferSnapshot(other.bufferSnapshot.exchange(std::make_shared<BufferSnapshot>()))
    , tempInputBuffer(std::move(other.tempInputBuffer))
    , tempOutputBuffer(std::move(other.tempOutputBuffer))
    , tempInputPointers(std::move(other.tempInputPointers))
    , tempOutputPointers(std::move(other.tempOutputPointers))
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
        clientBufferSize = other.clientBufferSize;
        bufferSnapshot.store(other.bufferSnapshot.exchange(std::make_shared<BufferSnapshot>()));
        tempInputBuffer = std::move(other.tempInputBuffer);
        tempOutputBuffer = std::move(other.tempOutputBuffer);
        tempInputPointers = std::move(other.tempInputPointers);
        tempOutputPointers = std::move(other.tempOutputPointers);

        other.clientId = nullptr;
    }
    return *this;
}

void AudioClient::pullSubscribedInputs(juce::AudioBuffer<float>& deviceBuffer, int numSamples, double sampleRate)
{
    auto snapshot = bufferSnapshot.load(std::memory_order_acquire);
    if (!snapshot || snapshot->inputGroups.empty())
    {
        deviceBuffer.clear();
        return;
    }

    int numSubs = static_cast<int>(snapshot->inputBuffers.size());
    if (deviceBuffer.getNumChannels() < numSubs)
    {
        deviceBuffer.clear();
        return;
    }

    deviceBuffer.clear();

    // Use pre-grouped buffers (no runtime allocation)
    for (const auto& group : snapshot->inputGroups)
    {
        if (!group.buffer)
            continue;

        int numDevCh = group.maxDeviceChannel + 1;

        // Use pre-allocated temp buffer (should already be sized from audioDeviceAboutToStart)
        // Only resize if absolutely necessary (this path should rarely be hit)
        if (tempInputBuffer.getNumChannels() < numDevCh || tempInputBuffer.getNumSamples() < numSamples)
            tempInputBuffer.setSize(numDevCh, numSamples, false, false, true);

        if (static_cast<int>(tempInputPointers.size()) < numDevCh)
            tempInputPointers.resize(numDevCh);

        for (int ch = 0; ch < numDevCh; ++ch)
            tempInputPointers[ch] = tempInputBuffer.getWritePointer(ch);

        if (group.buffer->read(tempInputPointers.data(), numDevCh, numSamples, sampleRate))
        {
            for (const auto& [subIdx, devCh] : group.channelMap)
                if (devCh < numDevCh && subIdx < deviceBuffer.getNumChannels())
                    deviceBuffer.copyFrom(subIdx, 0, tempInputBuffer, devCh, 0, numSamples);
        }
    }
}

void AudioClient::pushSubscribedOutputs(const juce::AudioBuffer<float>& deviceBuffer, int numSamples, double sampleRate)
{
    auto snapshot = bufferSnapshot.load(std::memory_order_acquire);
    if (!snapshot || snapshot->outputGroups.empty())
        return;

    int numSubs = static_cast<int>(snapshot->outputBuffers.size());
    if (deviceBuffer.getNumChannels() < numSubs)
        return;

    // Use pre-grouped buffers (no runtime allocation)
    for (const auto& group : snapshot->outputGroups)
    {
        if (!group.buffer)
            continue;

        int numDevCh = group.maxDeviceChannel + 1;

        // Use pre-allocated temp buffer (should already be sized)
        if (tempOutputBuffer.getNumChannels() < numDevCh || tempOutputBuffer.getNumSamples() < numSamples)
            tempOutputBuffer.setSize(numDevCh, numSamples, false, false, true);

        if (static_cast<int>(tempOutputPointers.size()) < numDevCh)
            tempOutputPointers.resize(numDevCh);

        tempOutputBuffer.clear(0, numSamples);

        for (const auto& [subIdx, devCh] : group.channelMap)
            if (subIdx < deviceBuffer.getNumChannels() && devCh < numDevCh)
                tempOutputBuffer.copyFrom(devCh, 0, deviceBuffer, subIdx, 0, numSamples);

        for (int ch = 0; ch < numDevCh; ++ch)
            tempOutputPointers[ch] = tempOutputBuffer.getReadPointer(ch);

        group.buffer->write(tempOutputPointers.data(), numDevCh, numSamples, sampleRate);
    }
}

void AudioClient::clearBuffers()
{
    auto snapshot = bufferSnapshot.load(std::memory_order_acquire);
    if (!snapshot)
        return;

    // Clear all input buffers
    for (const auto& bufRef : snapshot->inputBuffers)
        if (bufRef.buffer)
            bufRef.buffer->reset();

    // Clear all output buffers
    for (const auto& bufRef : snapshot->outputBuffers)
        if (bufRef.buffer)
            bufRef.buffer->reset();
}

void AudioClient::setSubscriptions(const AudioClientState& state)
{
    if (auto* server = AudioServer::getInstanceWithoutCreating())
        server->updateClientSubscriptions(clientId, state);
}

AudioClientState AudioClient::getSubscriptions() const
{
    auto snapshot = bufferSnapshot.load(std::memory_order_acquire);
    return snapshot ? snapshot->state : AudioClientState();
}

int AudioClient::getNumInputSubscriptions() const
{
    auto snapshot = bufferSnapshot.load(std::memory_order_acquire);
    return snapshot ? static_cast<int>(snapshot->inputBuffers.size()) : 0;
}

int AudioClient::getNumOutputSubscriptions() const
{
    auto snapshot = bufferSnapshot.load(std::memory_order_acquire);
    return snapshot ? static_cast<int>(snapshot->outputBuffers.size()) : 0;
}

void AudioClient::updateBufferSnapshot(std::shared_ptr<BufferSnapshot> newSnapshot)
{
    bufferSnapshot.store(std::move(newSnapshot), std::memory_order_release);
}

void AudioClient::ensureTempBufferCapacity(int numChannels, int numSamples)
{
    // Pre-allocate temp buffers to avoid allocations on audio path
    // Called from non-realtime context when subscriptions change
    if (tempInputBuffer.getNumChannels() < numChannels || tempInputBuffer.getNumSamples() < numSamples)
        tempInputBuffer.setSize(numChannels, numSamples, false, false, true);

    if (tempOutputBuffer.getNumChannels() < numChannels || tempOutputBuffer.getNumSamples() < numSamples)
        tempOutputBuffer.setSize(numChannels, numSamples, false, false, true);

    if (static_cast<int>(tempInputPointers.size()) < numChannels)
        tempInputPointers.resize(numChannels);

    if (static_cast<int>(tempOutputPointers.size()) < numChannels)
        tempOutputPointers.resize(numChannels);
}

// AudioDeviceHandler

AudioDeviceHandler::AudioDeviceHandler(const juce::String& name, const juce::String& key)
    : deviceName(name)
    , deviceKey(key)
    , device(nullptr)
{
}

AudioDeviceHandler::~AudioDeviceHandler()
{
    closeDevice();
}

bool AudioDeviceHandler::openDevice()
{
    bool wasAlreadyOpen = isDeviceOpen();

    if (wasAlreadyOpen)
    {
        // Device already open, nothing to do
        clearRecoveryPending();
        return true;
    }

    atk::logging::debug("AudioDeviceHandler::openDevice", "opening device \"" + deviceName + "\"");

    // Get the enumerator to find device type
    auto* server = AudioServer::getInstanceWithoutCreating();
    if (!server)
    {
        atk::logging::warning("AudioDeviceHandler::openDevice", "AudioServer instance not available");
        markRecoveryPending();
        return false;
    }

    device = server->createNativeDevice(deviceKey);
    if (!device)
    {
        atk::logging::warning("AudioDeviceHandler::openDevice", "failed to create device \"" + deviceName + "\"");
        markRecoveryPending();
        return false;
    }

    auto setupRevision = desiredSetupRevision.load(std::memory_order_acquire);
    auto options = getPreferredRecoverySetup();
    if (options.sampleRate <= 0.0)
        options.sampleRate = device->getCurrentSampleRate();
    if (options.bufferSize <= 0)
        options.bufferSize = device->getDefaultBufferSize();

    // Prepare channel configuration
    juce::BigInteger inputChannels;
    juce::BigInteger outputChannels;
    inputChannels.setRange(0, device->getInputChannelNames().size(), true);
    outputChannels.setRange(0, device->getOutputChannelNames().size(), true);

    // Open the device
    atk::logging::info("AudioDeviceHandler::openDevice", "driver open begin: " + deviceKey);
    juce::String error = device->open(inputChannels, outputChannels, options.sampleRate, options.bufferSize);
    atk::logging::info("AudioDeviceHandler::openDevice", "driver open completed: " + deviceKey + " error=" + error);

    if (!error.isEmpty())
    {
        atk::logging::warning(
            "AudioDeviceHandler::openDevice",
            "failed to open device \"" + deviceName + "\": " + error
        );
        device.reset();
        markRecoveryPending();
        return false;
    }

    // Start the device with our callback
    device->start(this);

    // Populate cache with actual device channel info
    if (server)
    {
        server->cacheDeviceInfo(
            deviceKey,
            device->getInputChannelNames(),
            device->getOutputChannelNames(),
            device->getAvailableSampleRates(),
            device->getAvailableBufferSizes()
        );
    }

    // Check if device actually started
    if (!device->isPlaying())
    {
        atk::logging::warning("AudioDeviceHandler::openDevice", "device \"" + deviceName + "\" failed to start playing");
        device->close();
        device.reset();
        markRecoveryPending();
        return false;
    }

    atk::logging::info("AudioDeviceHandler::openDevice", "opened device \"" + deviceName + "\"");
    currentSampleRate.store(device->getCurrentSampleRate(), std::memory_order_release);
    currentBufferSize.store(device->getCurrentBufferSizeSamples(), std::memory_order_release);
    currentChannelCount.store(
        std::max(inputChannels.countNumberOfSetBits(), outputChannels.countNumberOfSetBits()),
        std::memory_order_release
    );
    deviceOpen.store(true, std::memory_order_release);
    reportNegotiatedSetup("open");
    appliedSetupRevision.store(setupRevision, std::memory_order_release);
    clearRecoveryPending();

    return true;
}

void AudioDeviceHandler::closeDevice()
{
    deviceOpen.store(false, std::memory_order_release);
    if (!device)
        return;

    device->stop();
    device->close();
    device.reset();
    currentSampleRate.store(0.0, std::memory_order_release);
    currentBufferSize.store(0, std::memory_order_release);
    currentChannelCount.store(0, std::memory_order_release);
    if (auto* server = AudioServer::getInstanceWithoutCreating())
        server->invalidateDeviceCache(deviceKey);
}

bool AudioDeviceHandler::isDeviceOpen() const
{
    return deviceOpen.load(std::memory_order_acquire);
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

                        if (buffers.outputBuffer
                                ->read(rtSubscriptionPointers.data(), numDeviceChannels, numSamples, deviceSampleRate))
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
    auto directSnapshot = directCallbackSnapshot.load(std::memory_order_acquire);
    if (directSnapshot && !directSnapshot->callbacks.empty())
    {
        for (const auto& info : directSnapshot->callbacks)
        {
            if (!info)
                continue;

            info->activeCalls.fetch_add(1, std::memory_order_acq_rel);

            struct CallbackGuard
            {
                DirectCallbackInfo* info;

                ~CallbackGuard()
                {
                    info->activeCalls.fetch_sub(1, std::memory_order_release);
                }
            } guard{info.get()};

            if (!info->enabled.load(std::memory_order_acquire)
                || !info->prepared.load(std::memory_order_acquire)
                || info->callback == nullptr)
                continue;

            // Skip if temp buffer not sized correctly
            if (numOutputChannels > info->tempOutputBuffer.getNumChannels()
                || numSamples > info->tempOutputBuffer.getNumSamples())
                continue;

            // Skip if output pointers vector not sized correctly (safety check)
            if (numOutputChannels > static_cast<int>(info->outputPointers.size()))
                continue;

            info->tempOutputBuffer.clear();
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
    int maxChannels = std::max(
        device->getActiveInputChannels().countNumberOfSetBits(),
        device->getActiveOutputChannels().countNumberOfSetBits()
    );
    int bufferSize = device->getCurrentBufferSizeSamples();
    currentSampleRate.store(device->getCurrentSampleRate(), std::memory_order_release);
    currentBufferSize.store(bufferSize, std::memory_order_release);
    currentChannelCount.store(maxChannels, std::memory_order_release);

    rtSubscriptionTempBuffer.setSize(maxChannels, bufferSize, false, false, true);
    rtSubscriptionPointers.resize(maxChannels);
    rtInputPointers.resize(maxChannels);

    {
        std::lock_guard<std::mutex> lock(directCallbackMutex);
        for (auto& entry : directCallbacks)
        {
            prepareDirectCallback(*entry.second, *device);
            entry.first->audioDeviceAboutToStart(device);
            entry.second->prepared.store(true, std::memory_order_release);
        }
        rebuildDirectCallbackSnapshotLocked();
    }

    if (auto* server = AudioServer::getInstanceWithoutCreating())
    {
        server->cacheDeviceInfo(
            deviceKey,
            device->getInputChannelNames(),
            device->getOutputChannelNames(),
            device->getAvailableSampleRates(),
            device->getAvailableBufferSizes()
        );
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputChannels = device->getActiveInputChannels();
        setup.outputChannels = device->getActiveOutputChannels();
        setup.inputDeviceName = setup.inputChannels.isZero() ? juce::String() : deviceName;
        setup.outputDeviceName = setup.outputChannels.isZero() ? juce::String() : deviceName;
        setup.sampleRate = device->getCurrentSampleRate();
        setup.bufferSize = device->getCurrentBufferSizeSamples();
        server->cacheDeviceSetup(deviceKey, setup);
    }

    // Check if we have active subscriptions and enable processing
    {
        std::lock_guard<std::mutex> lock(clientBuffersMutex);
        if (!clientBuffers.empty())
            isRunning.store(true, std::memory_order_release);
    }
}

void AudioDeviceHandler::prepareDirectCallback(DirectCallbackInfo& info, juce::AudioIODevice& device)
{
    int outputChannels = device.getActiveOutputChannels().countNumberOfSetBits();
    int bufferSize = device.getCurrentBufferSizeSamples();

    info.tempOutputBuffer.setSize(outputChannels, bufferSize, false, false, true);
    info.outputPointers.resize(outputChannels);
}

void AudioDeviceHandler::preparePendingDirectCallbacks()
{
    if (!isDeviceOpen() || !device)
        return;
    std::lock_guard<std::mutex> lock(directCallbackMutex);
    for (auto& entry : directCallbacks)
    {
        if (entry.second->prepared.load(std::memory_order_acquire))
            continue;
        prepareDirectCallback(*entry.second, *device);
        entry.first->audioDeviceAboutToStart(device.get());
        entry.second->prepared.store(true, std::memory_order_release);
    }
}

void AudioDeviceHandler::audioDeviceStopped()
{
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

    if (!device)
        return;

    // Invalidate and update cache with new device info
    // Note: JUCE will call audioDeviceStopped() and audioDeviceAboutToStart()
    // when the device restarts, which will handle buffer reallocation
    if (auto* server = AudioServer::getInstanceWithoutCreating())
    {
        server->invalidateDeviceCache(deviceKey);
        server->cacheDeviceInfo(
            deviceKey,
            device->getInputChannelNames(),
            device->getOutputChannelNames(),
            device->getAvailableSampleRates(),
            device->getAvailableBufferSizes()
        );
    }

    reportNegotiatedSetup("device_changed");
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
        atk::logging::debug(
            "AudioDeviceHandler::addClientSubscription",
            "lazy-opening device \"" + deviceName + "\" for " + (isInput ? "input" : "output") + " subscription"
        );
        // Use empty setup to let openDevice use device defaults
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.sampleRate = 0.0; // Signal to use device default
        setup.bufferSize = 0;   // Signal to use device default

        updatePreferredRecoverySetup(setup);
        {
            std::lock_guard<std::mutex> lock(clientBuffersMutex);
            auto& desired = desiredClientSubscriptions[clientId];
            if (isInput)
                desired.hasInput = true;
            else
                desired.hasOutput = true;
        }
        if (auto* server = AudioServer::getInstanceWithoutCreating())
            server->requestDeviceActivation(deviceKey);
        return;
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
            buffers.inputBuffer = std::make_shared<SyncBuffer>(deviceName + " in");

            atk::logging::debug(
                "AudioDeviceHandler::addClientSubscription",
                "created input SyncBuffer for \"" + deviceName + "\""
            );
        }
    }
    else
    {
        buffers.outputMappings = mappings;

        // Create single multichannel SyncBuffer for all device output channels
        if (!buffers.outputBuffer)
        {
            buffers.outputBuffer = std::make_shared<SyncBuffer>(deviceName + " out");

            atk::logging::debug(
                "AudioDeviceHandler::addClientSubscription",
                "created output SyncBuffer for \"" + deviceName + "\""
            );
        }
    }

    // Rebuild snapshot so audio callback sees new buffers
    rebuildSnapshotLocked();

    // NOW it's safe to allow callbacks - all SyncBuffers are created
    if (justOpened)
    {
        isRunning.store(true, std::memory_order_release);
        atk::logging::debug(
            "AudioDeviceHandler::addClientSubscription",
            "enabled processing for \"" + deviceName + "\""
        );
    }
}

void AudioDeviceHandler::removeClientSubscription(void* clientId, bool isInput)
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

    auto desiredIt = desiredClientSubscriptions.find(clientId);
    if (desiredIt != desiredClientSubscriptions.end())
    {
        if (isInput)
            desiredIt->second.hasInput = false;
        else
            desiredIt->second.hasOutput = false;

        if (!desiredIt->second.hasInput && !desiredIt->second.hasOutput)
            desiredClientSubscriptions.erase(desiredIt);
    }

    // Rebuild snapshot to reflect changes.
    // Device lifetime is server-owned and not tied to temporary subscription gaps.
    rebuildSnapshotLocked();
}

bool AudioDeviceHandler::hasActiveSubscriptions() const
{
    std::lock_guard<std::mutex> lock(clientBuffersMutex);
    return !clientBuffers.empty() || hasDirectCallback();
}

bool AudioDeviceHandler::hasDesiredSubscriptions() const
{
    std::lock_guard<std::mutex> lock(clientBuffersMutex);
    if (!desiredClientSubscriptions.empty())
        return true;

    return hasDirectCallback();
}

bool AudioDeviceHandler::registerDirectCallback(juce::AudioIODeviceCallback* callback)
{
    if (callback == nullptr)
        return false;

    auto info = std::make_shared<DirectCallbackInfo>();
    info->callback = callback;

    {
        std::lock_guard<std::mutex> lock(directCallbackMutex);

        if (directCallbacks.find(callback) != directCallbacks.end())
        {
            atk::logging::warning(
                "AudioDeviceHandler::registerDirectCallback",
                "duplicate direct callback for device \"" + deviceName + "\""
            );
            return false;
        }

        directCallbacks[callback] = std::move(info);
        rebuildDirectCallbackSnapshotLocked();
    }

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
        auto retired = it->second;
        retired->enabled.store(false, std::memory_order_release);
        atk::logging::debug(
            "AudioDeviceHandler::unregisterDirectCallback",
            "removed direct callback for device \"" + deviceName + "\""
        );
        directCallbacks.erase(it);
        rebuildDirectCallbackSnapshotLocked();
        while (retired->activeCalls.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
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
        newSnapshot->callbacks.push_back(info);

    // Atomic publish - audio callback can now see new snapshot
    directCallbackSnapshot.store(newSnapshot, std::memory_order_release);
}

void AudioDeviceHandler::updatePreferredRecoverySetup(const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup)
{
    std::lock_guard<std::mutex> lock(setupMutex);
    if ((preferredSetup.sampleRate > 0.0 && preferredSetup.sampleRate != preferredRecoverySetup.sampleRate)
        || (preferredSetup.bufferSize > 0 && preferredSetup.bufferSize != preferredRecoverySetup.bufferSize))
        desiredSetupRevision.fetch_add(1, std::memory_order_release);
    if (!hasPreferredRecoverySetup)
    {
        preferredRecoverySetup = preferredSetup;
        hasPreferredRecoverySetup = true;
        return;
    }

    if (preferredSetup.sampleRate > 0.0)
        preferredRecoverySetup.sampleRate = preferredSetup.sampleRate;

    if (preferredSetup.bufferSize > 0)
        preferredRecoverySetup.bufferSize = preferredSetup.bufferSize;

    preferredRecoverySetup.inputChannels = preferredSetup.inputChannels;
    preferredRecoverySetup.outputChannels = preferredSetup.outputChannels;

    if (preferredSetup.inputDeviceName.isNotEmpty())
        preferredRecoverySetup.inputDeviceName = preferredSetup.inputDeviceName;

    if (preferredSetup.outputDeviceName.isNotEmpty())
        preferredRecoverySetup.outputDeviceName = preferredSetup.outputDeviceName;

    preferredRecoverySetup.useDefaultInputChannels = preferredSetup.useDefaultInputChannels;
    preferredRecoverySetup.useDefaultOutputChannels = preferredSetup.useDefaultOutputChannels;
}

juce::AudioDeviceManager::AudioDeviceSetup AudioDeviceHandler::getPreferredRecoverySetup() const
{
    std::lock_guard<std::mutex> lock(setupMutex);
    if (!hasPreferredRecoverySetup)
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.sampleRate = 0.0;
        setup.bufferSize = 0;
        return setup;
    }

    return preferredRecoverySetup;
}

bool AudioDeviceHandler::canAttemptRecovery(double nowMs, double retryIntervalMs)
{
    if (nowMs - lastRecoveryAttemptMs < retryIntervalMs)
        return false;

    lastRecoveryAttemptMs = nowMs;
    return true;
}

void AudioDeviceHandler::markRecoveryPending()
{
    recoveryPending.store(true, std::memory_order_release);
}

void AudioDeviceHandler::clearRecoveryPending()
{
    recoveryPending.store(false, std::memory_order_release);
}

void AudioDeviceHandler::resetSubscriptionBuffersAfterRecovery()
{
    std::lock_guard<std::mutex> lock(clientBuffersMutex);

    for (auto& [clientId, buffers] : clientBuffers)
    {
        juce::ignoreUnused(clientId);

        if (buffers.inputBuffer)
            buffers.inputBuffer->reset();

        if (buffers.outputBuffer)
            buffers.outputBuffer->reset();
    }
}

int AudioDeviceHandler::getNumChannels() const
{
    return currentChannelCount.load(std::memory_order_acquire);
}

double AudioDeviceHandler::getSampleRate() const
{
    return currentSampleRate.load(std::memory_order_acquire);
}

int AudioDeviceHandler::getBufferSize() const
{
    return currentBufferSize.load(std::memory_order_acquire);
}

void AudioDeviceHandler::reportNegotiatedSetup(const juce::String& context) const
{
    if (!device)
        return;

    auto sizes = device->getAvailableBufferSizes();

    atk::logging::info(
        "AudioDeviceHandler::reportNegotiatedSetup",
        "\""
            + deviceName
            + "\" ("
            + context
            + ") negotiated "
            + juce::String(device->getCurrentSampleRate(), 2)
            + " Hz / "
            + juce::String(device->getCurrentBufferSizeSamples())
            + " samples, available sizes ["
            + juce::String(sizes.isEmpty() ? 0 : sizes.getFirst())
            + ".."
            + juce::String(sizes.isEmpty() ? 0 : sizes.getLast())
            + "]"
    );
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

// AudioServer

JUCE_IMPLEMENT_SINGLETON(AudioServer)

AudioServer::AudioServer()
{
}

AudioServer::~AudioServer()
{
    atk::logging::info("AudioServer::dtor", "begin");
    shutdown();
    clearSingletonInstance();
    atk::logging::info("AudioServer::dtor", "completed");
}

juce::AudioDeviceManager* AudioServer::ensureDeviceEnumerator() const
{
    std::lock_guard<std::mutex> lock(deviceEnumeratorMutex);
    return deviceEnumerator.get();
}

std::unique_ptr<juce::AudioIODevice> AudioServer::createNativeDevice(const juce::String& deviceKey)
{
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (!messageManager || !messageManager->isThisTheMessageThread())
    {
        atk::logging::error("AudioServer::createNativeDevice", "native device creation requires the message thread");
        return {};
    }
    atk::logging::info("AudioServer::createNativeDevice", "backend lookup begin: " + deviceKey);
    auto* manager = ensureDeviceEnumerator();
    if (!manager)
        return {};

    int separator = deviceKey.indexOf("|");
    auto backend = separator >= 0 ? deviceKey.substring(0, separator) : juce::String();
    auto name = separator >= 0 ? deviceKey.substring(separator + 1) : deviceKey;
    juce::AudioIODeviceType* selectedType = nullptr;
    for (auto* type : manager->getAvailableDeviceTypes())
    {
        if (backend.isNotEmpty() && type->getTypeName() != backend)
            continue;
        if (!type->getDeviceNames(true).contains(name) && !type->getDeviceNames(false).contains(name))
            continue;
        if (selectedType != nullptr)
        {
            atk::logging::warning("AudioServer::createNativeDevice", "ambiguous device: " + deviceKey);
            return {};
        }
        selectedType = type;
    }
    if (!selectedType)
        return {};

    atk::logging::info("AudioServer::createNativeDevice", "driver create begin: " + deviceKey);
    std::unique_ptr<juce::AudioIODevice> result(selectedType->createDevice(
        selectedType->getDeviceNames(false).contains(name) ? name : juce::String(),
        selectedType->getDeviceNames(true).contains(name) ? name : juce::String()
    ));
    atk::logging::info("AudioServer::createNativeDevice", "driver create completed: " + deviceKey);
    return result;
}

void AudioServer::setupDeviceEnumeratorListeners()
{
    if (!deviceEnumerator)
        return;

    deviceEnumerator->addChangeListener(const_cast<AudioServer*>(this));
}

void AudioServer::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == deviceEnumerator.get())
    {
        scanRequested.store(true, std::memory_order_release);
        startTimer(1);
    }
}

void AudioServer::timerCallback()
{
    if (!initialized.load(std::memory_order_acquire) || processingDeviceRequests)
        return;
    juce::ScopedValueSetter<bool> processing(processingDeviceRequests, true);
    startTimer(100);
    if (scanRequested.exchange(false, std::memory_order_acq_rel))
        scanDeviceCatalog();
    attemptPendingDeviceRecovery("message_thread");

    bool changedCatalog = catalogChanged.exchange(false, std::memory_order_acq_rel);
    bool refreshClients = devicesChanged.exchange(false, std::memory_order_acq_rel) || changedCatalog;
    if (refreshClients)
    {
        std::vector<std::pair<void*, AudioClientState>> states;
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (auto& entry : clients)
                states.push_back({entry.first, *entry.second.state.load()});
        }
        for (auto& entry : states)
            updateClientSubscriptions(entry.first, entry.second);
    }
    if (refreshClients)
        listeners.call([](Listener& listener) { listener.audioServerDeviceListChanged(); });
}

void AudioServer::scanDeviceCatalog()
{
    auto* manager = ensureDeviceEnumerator();
    if (!manager)
        return;
    atk::logging::info("AudioServer::scanDeviceCatalog", "native scan begin");
    std::map<juce::String, juce::StringArray> inputs;
    std::map<juce::String, juce::StringArray> outputs;
    bool changed = false;
    for (auto* type : manager->getAvailableDeviceTypes())
    {
        auto inputNames = type->getDeviceNames(true);
        auto outputNames = type->getDeviceNames(false);
        if (!inputNames.isEmpty())
            inputs[type->getTypeName()] = inputNames;
        if (!outputNames.isEmpty())
            outputs[type->getTypeName()] = outputNames;
    }
    {
        std::lock_guard<std::mutex> lock(catalogMutex);
        changed = inputDeviceCatalog != inputs || outputDeviceCatalog != outputs;
        inputDeviceCatalog = std::move(inputs);
        outputDeviceCatalog = std::move(outputs);
    }
    {
        std::lock_guard<std::mutex> lock(deviceTypeCacheMutex);
        deviceNameToTypeCache.clear();
        for (auto* type : manager->getAvailableDeviceTypes())
        {
            auto names = type->getDeviceNames(true);
            names.addArray(type->getDeviceNames(false));
            names.removeDuplicates(false);
            for (auto& name : names)
            {
                auto found = deviceNameToTypeCache.find(name);
                if (found == deviceNameToTypeCache.end())
                    deviceNameToTypeCache[name] = type->getTypeName();
                else if (found->second != type->getTypeName())
                    found->second.clear();
            }
        }
    }
    catalogReady.store(true, std::memory_order_release);
    if (changed)
        catalogChanged.store(true, std::memory_order_release);
    atk::logging::info("AudioServer::scanDeviceCatalog", "native scan completed");
}

void AudioServer::requestDeviceActivation(const juce::String& deviceKey)
{
    if (!initialized.load(std::memory_order_acquire) || deviceKey.isEmpty())
        return;
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        getOrCreateDeviceHandler(deviceKey)->activationRequested.store(true, std::memory_order_release);
    }
    startTimer(1);
}

void AudioServer::addListener(Listener* listener)
{
    listeners.add(listener);
}

void AudioServer::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

void AudioServer::initialize(std::unique_ptr<juce::AudioDeviceManager> nativeDeviceManager)
{
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (!messageManager || !messageManager->isThisTheMessageThread())
    {
        atk::logging::error("AudioServer::initialize", "initialization requires the message thread");
        return;
    }
    if (initialized.load(std::memory_order_acquire))
        return;

    atk::logging::info("AudioServer::initialize", "begin");
    deviceEnumerator =
        nativeDeviceManager ? std::move(nativeDeviceManager) : std::make_unique<juce::AudioDeviceManager>();
    setupDeviceEnumeratorListeners();

    initialized.store(true, std::memory_order_release);
    startTimer(100);

    atk::logging::info("AudioServer::initialize", "completed");
}

void AudioServer::shutdown()
{
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (!messageManager || !messageManager->isThisTheMessageThread())
    {
        atk::logging::error("AudioServer::shutdown", "shutdown requires the message thread");
        return;
    }
    if (!initialized.load(std::memory_order_acquire))
        return;

    atk::logging::info("AudioServer::shutdown", "begin");

    initialized.store(false, std::memory_order_release);
    stopTimer();

    std::unordered_map<juce::String, std::shared_ptr<AudioDeviceHandler>> retired;
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        retired.swap(deviceHandlers);
    }
    retired.clear();

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
        inputDeviceChannelNamesCache.clear();
        outputDeviceChannelNamesCache.clear();
    }

    // Clear device type cache
    {
        std::lock_guard<std::mutex> lock(deviceTypeCacheMutex);
        deviceNameToTypeCache.clear();
    }

    deviceEnumerator.reset();
    catalogReady.store(false, std::memory_order_release);
    scanRequested.store(false, std::memory_order_release);
    catalogChanged.store(false, std::memory_order_release);
    devicesChanged.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(catalogMutex);
        inputDeviceCatalog.clear();
        outputDeviceCatalog.clear();
    }
    {
        std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
        deviceSetupCache.clear();
        deviceSampleRatesCache.clear();
        deviceBufferSizesCache.clear();
    }

    atk::logging::info("AudioServer::shutdown", "completed");
}

bool AudioServer::shouldRecoverDevice(AudioDeviceHandler* handler) const
{
    if (handler == nullptr)
        return false;

    if (!handler->activationRequested.load(std::memory_order_acquire)
        && !handler->hasActiveSubscriptions()
        && !handler->hasDesiredSubscriptions())
        return false;

    if (handler->isRecoveryPending())
        return true;

    if (!handler->isDeviceOpen())
        return true;

    auto* device = handler->getCurrentDevice();
    if (device == nullptr)
        return true;

    if (!device->isOpen())
        return true;

    return false;
}

void AudioServer::attemptPendingDeviceRecovery(const juce::String& reason)
{
    if (!initialized.load(std::memory_order_acquire))
        return;

    std::vector<std::shared_ptr<AudioDeviceHandler>> handlers;
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        for (auto& entry : deviceHandlers)
            handlers.push_back(entry.second);
    }

    double nowMs = juce::Time::getMillisecondCounterHiRes();

    for (auto& handler : handlers)
    {
        if (!initialized.load(std::memory_order_acquire))
            return;
        handler->preparePendingDirectCallbacks();
        if (!shouldRecoverDevice(handler.get()))
            continue;

        if (!handler->canAttemptRecovery(nowMs, recoveryRetryIntervalMs))
            continue;

        atk::logging::debug(
            "AudioServer::attemptPendingDeviceRecovery",
            "attempting recovery for \"" + handler->getDeviceName() + "\" reason=" + reason
        );

        if (handler->isDeviceOpen())
            handler->closeDevice();

        if (handler->openDevice())
        {
            handler->resetSubscriptionBuffersAfterRecovery();
            devicesChanged.store(true, std::memory_order_release);
            atk::logging::info(
                "AudioServer::attemptPendingDeviceRecovery",
                "recovered device \"" + handler->getDeviceName() + "\""
            );
        }
    }
}

juce::StringArray AudioServer::getOpenDeviceNames() const
{
    std::lock_guard<std::mutex> lock(devicesMutex);
    juce::StringArray names;
    for (const auto& [key, handler] : deviceHandlers)
    {
        int sep = key.indexOf("|");
        juce::String deviceName = (sep >= 0) ? key.substring(sep + 1) : key;
        if (handler->isDeviceOpen() && deviceName.isNotEmpty())
            names.addIfNotAlreadyThere(deviceName);
    }
    return names;
}

juce::String AudioServer::makeDeviceKey(const juce::String& deviceType, const juce::String& deviceName)
{
    if (deviceType.isEmpty())
        return deviceName;

    return deviceType + "|" + deviceName;
}

juce::String AudioServer::resolveDeviceKeyForSubscription(const ChannelSubscription& sub) const
{
    if (sub.deviceName.isEmpty())
        return {};

    if (sub.deviceType.isNotEmpty())
        return makeDeviceKey(sub.deviceType, sub.deviceName);

    auto key = findDeviceKeyByName(sub.deviceName);
    if (key == sub.deviceName)
    {
        if (!catalogReady.load(std::memory_order_acquire))
        {
            scanRequested.store(true, std::memory_order_release);
            const_cast<AudioServer*>(this)->startTimer(1);
        }
        return {};
    }
    return key;
}

juce::String AudioServer::findDeviceKeyByName(const juce::String& deviceName) const
{
    if (deviceName.contains("|"))
        return deviceName;

    // Check device type cache (avoids slow scanForDevices)
    {
        std::lock_guard<std::mutex> lock(deviceTypeCacheMutex);
        auto it = deviceNameToTypeCache.find(deviceName);
        if (it != deviceNameToTypeCache.end() && it->second.isNotEmpty())
            return makeDeviceKey(it->second, deviceName);
    }

    // Fallback: use deviceName as key (legacy compatibility)
    return deviceName;
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
        info.clientPtr = static_cast<AudioClient*>(clientId);
        info.state.store(std::make_shared<AudioClientState>(state));
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

        // Remove all client subscriptions from all handlers.
        // Handlers stay alive to avoid open/close churn across temporary subscription gaps.
        for (auto& [name, handler] : deviceHandlers)
        {
            handler->removeClientSubscription(clientId, true);  // Remove input subscriptions
            handler->removeClientSubscription(clientId, false); // Remove output subscriptions
        }
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

    bool stateUnchanged = false;

    // Check if subscriptions have actually changed
    {
        std::lock_guard<std::mutex> lock(clientsMutex);

        auto it = clients.find(clientId);
        if (it == clients.end())
            return;

        auto currentState = it->second.state.load();
        if (currentState && *currentState == state)
            stateUnchanged = true;

        // Update state atomically (old shared_ptr deleted automatically when refcount hits 0)
        if (!stateUnchanged)
            it->second.state.store(std::make_shared<AudioClientState>(state));
    }

    atk::logging::debug("AudioServer::updateClientSubscriptions", "applying updated client subscriptions");
    // Update device handlers (acquire devicesMutex AFTER releasing clientsMutex to avoid deadlock)
    std::lock_guard<std::mutex> deviceLock(devicesMutex);

    if (stateUnchanged)
    {
        bool needsReapply = false;

        auto hasReadyDirection = [clientId](AudioDeviceHandler* handler, bool isInput) -> bool
        {
            if (handler == nullptr)
                return false;

            std::lock_guard<std::mutex> handlerLock(handler->getClientBuffersMutex());

            auto& desiredClientSubscriptions = handler->getDesiredClientSubscriptions();
            auto desiredIt = desiredClientSubscriptions.find(clientId);
            if (desiredIt == desiredClientSubscriptions.end())
                return false;

            bool expectsDirection = isInput ? desiredIt->second.hasInput : desiredIt->second.hasOutput;
            if (!expectsDirection)
                return true;

            auto& clientBuffers = handler->getClientBuffers();
            auto bufferIt = clientBuffers.find(clientId);
            if (bufferIt == clientBuffers.end())
                return false;

            if (isInput)
                return bufferIt->second.inputBuffer != nullptr && !bufferIt->second.inputMappings.empty();

            return bufferIt->second.outputBuffer != nullptr && !bufferIt->second.outputMappings.empty();
        };

        for (const auto& sub : state.inputSubscriptions)
        {
            auto deviceKey = resolveDeviceKeyForSubscription(sub);
            auto it = deviceHandlers.find(deviceKey);

            if (it == deviceHandlers.end() || !hasReadyDirection(it->second.get(), true))
            {
                needsReapply = true;
                break;
            }
        }

        if (!needsReapply)
            for (const auto& sub : state.outputSubscriptions)
            {
                auto deviceKey = resolveDeviceKeyForSubscription(sub);
                auto it = deviceHandlers.find(deviceKey);

                if (it == deviceHandlers.end() || !hasReadyDirection(it->second.get(), false))
                {
                    needsReapply = true;
                    break;
                }
            }

        if (!needsReapply)
        {
            atk::logging::debug("AudioServer::updateClientSubscriptions", "skipped (state unchanged)");
            return;
        }

        atk::logging::debug(
            "AudioServer::updateClientSubscriptions",
            "forcing reapply (state unchanged but runtime buffers missing)"
        );
    }

    // ATOMIC SWAP APPROACH: Build new subscription state, then swap atomically
    // Use composite device keys (deviceType|deviceName) to avoid conflicts

    // Step 1: Group NEW subscriptions by device key
    std::unordered_map<juce::String, std::vector<ChannelSubscription>> newInputSubs;
    std::unordered_map<juce::String, std::vector<ChannelSubscription>> newOutputSubs;

    for (const auto& sub : state.inputSubscriptions)
    {
        auto deviceKey = resolveDeviceKeyForSubscription(sub);
        if (deviceKey.isEmpty())
            continue;

        newInputSubs[deviceKey].push_back(sub);
    }

    for (const auto& sub : state.outputSubscriptions)
    {
        auto deviceKey = resolveDeviceKeyForSubscription(sub);
        if (deviceKey.isEmpty())
            continue;

        newOutputSubs[deviceKey].push_back(sub);
    }

    // Step 2: Get set of ALL device keys (old + new)
    std::set<juce::String> allDeviceKeys;

    // Add device keys from new state
    for (const auto& [deviceKey, _] : newInputSubs)
        allDeviceKeys.insert(deviceKey);
    for (const auto& [deviceKey, _] : newOutputSubs)
        allDeviceKeys.insert(deviceKey);

    // Add device keys from old state (by checking existing handlers)
    std::vector<juce::String> existingDeviceKeys;
    for (auto& [key, handler] : deviceHandlers)
        existingDeviceKeys.push_back(key);

    // Check each handler's clientBuffers
    for (const auto& key : existingDeviceKeys)
    {
        auto it = deviceHandlers.find(key);
        if (it != deviceHandlers.end())
        {
            auto* handler = it->second.get();
            std::lock_guard<std::mutex> handlerLock(handler->getClientBuffersMutex());
            auto& clientBuffers = handler->getClientBuffers();
            if (clientBuffers.find(clientId) != clientBuffers.end())
                allDeviceKeys.insert(key);
        }
    }

    // Step 3: For each device, atomically update subscriptions
    for (const auto& deviceKey : allDeviceKeys)
    {
        auto* handler = getOrCreateDeviceHandler(deviceKey);
        if (!handler)
            continue;

        // Get new subscriptions for this device (empty if device no longer subscribed)
        auto newInputIt = newInputSubs.find(deviceKey);
        auto newOutputIt = newOutputSubs.find(deviceKey);

        std::vector<ChannelSubscription> newInput =
            (newInputIt != newInputSubs.end()) ? newInputIt->second : std::vector<ChannelSubscription>{};
        std::vector<ChannelSubscription> newOutput =
            (newOutputIt != newOutputSubs.end()) ? newOutputIt->second : std::vector<ChannelSubscription>{};

        // ATOMIC: Update subscriptions in single locked operation
        // IMPORTANT: Preserve existing SyncBuffers to avoid audio discontinuity
        {
            std::unique_lock<std::mutex> handlerLock(handler->getClientBuffersMutex());
            bool snapshotDirty = false;
            auto& clientBuffers = handler->getClientBuffers();
            auto& desiredClientSubscriptions = handler->getDesiredClientSubscriptions();

            if (newInput.empty() && newOutput.empty())
                desiredClientSubscriptions.erase(clientId);
            else
                desiredClientSubscriptions[clientId] = {
                    !newInput.empty(),
                    !newOutput.empty(),
                };

            auto bufferIt = clientBuffers.find(clientId);
            if (bufferIt != clientBuffers.end())
            {
                // If no new subscriptions for this device, remove client entry entirely
                if (newInput.empty() && newOutput.empty())
                {
                    bufferIt->second.inputBuffer.reset();
                    bufferIt->second.outputBuffer.reset();
                    bufferIt->second.inputMappings.clear();
                    bufferIt->second.outputMappings.clear();
                    clientBuffers.erase(bufferIt);
                    handler->rebuildSnapshotLocked();
                    continue;
                }

                // Preserve existing SyncBuffers - only clear mappings
                // This prevents audio discontinuity when adding/removing channels
                bufferIt->second.inputMappings.clear();
                bufferIt->second.outputMappings.clear();

                // Only reset buffers if subscription type is being removed entirely
                if (newInput.empty() && bufferIt->second.inputBuffer)
                {
                    bufferIt->second.inputBuffer.reset();
                    snapshotDirty = true;
                }
                if (newOutput.empty() && bufferIt->second.outputBuffer)
                {
                    bufferIt->second.outputBuffer.reset();
                    snapshotDirty = true;
                }
            }
            else if (newInput.empty() && newOutput.empty())
            {
                // No old subscriptions, no new subscriptions - skip
                continue;
            }

            // Add new input subscriptions
            if (!newInput.empty())
            {
                bool justOpened = false;
                if (!handler->isDeviceOpen())
                {
                    handler->markRecoveryPending();
                    startTimer(1);
                    continue;
                }

                auto& buffers = clientBuffers[clientId];

                std::vector<ChannelMapping> inputMappings;
                for (size_t i = 0; i < newInput.size(); ++i)
                {
                    ChannelMapping mapping;
                    mapping.deviceChannel = newInput[i];
                    mapping.clientChannel = static_cast<int>(i);
                    inputMappings.push_back(mapping);
                }
                buffers.inputMappings = inputMappings;

                if (!buffers.inputBuffer)
                    buffers.inputBuffer = std::make_shared<SyncBuffer>(handler->getDeviceName() + " in");

                snapshotDirty = true;

                if (justOpened || !handler->isRunningNow())
                    handler->setRunning(true);
            }

            // Add new output subscriptions
            if (!newOutput.empty())
            {
                bool justOpened = false;
                if (!handler->isDeviceOpen())
                {
                    handler->markRecoveryPending();
                    startTimer(1);
                    continue;
                }

                auto& buffers = clientBuffers[clientId];

                std::vector<ChannelMapping> outputMappings;
                for (size_t i = 0; i < newOutput.size(); ++i)
                {
                    ChannelMapping mapping;
                    mapping.deviceChannel = newOutput[i];
                    mapping.clientChannel = static_cast<int>(i);
                    outputMappings.push_back(mapping);
                }
                buffers.outputMappings = outputMappings;

                if (!buffers.outputBuffer)
                    buffers.outputBuffer = std::make_shared<SyncBuffer>(handler->getDeviceName() + " out");

                snapshotDirty = true;

                if (justOpened || !handler->isRunningNow())
                    handler->setRunning(true);
            }

            if (snapshotDirty)
                handler->rebuildSnapshotLocked();
        }
    }

    // Step 4: Rebuild client's buffer snapshot for lock-free audio access
    rebuildClientBufferSnapshot(clientId);

    atk::logging::debug("AudioServer::updateClientSubscriptions", "completed");
}

void AudioServer::rebuildClientBufferSnapshot(void* clientId)
{
    // Must be called while holding devicesMutex
    // Build new buffer snapshot for the client

    AudioClient* clientPtr = nullptr;
    AudioClientState currentState;

    // Get client info
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        auto it = clients.find(clientId);
        if (it == clients.end())
            return;

        clientPtr = it->second.clientPtr;
        auto statePtr = it->second.state.load();
        if (statePtr)
            currentState = *statePtr;
    }

    if (!clientPtr)
        return;

    auto newSnapshot = std::make_shared<AudioClient::BufferSnapshot>();
    newSnapshot->state = currentState;

    // Build input buffer refs and group by SyncBuffer for realtime-safe access
    std::unordered_map<SyncBuffer*, AudioClient::BufferGroup> inputGroupMap;
    for (size_t i = 0; i < currentState.inputSubscriptions.size(); ++i)
    {
        const auto& sub = currentState.inputSubscriptions[i];
        juce::String deviceKey = resolveDeviceKeyForSubscription(sub);
        if (deviceKey.isEmpty())
            continue;

        auto handlerIt = deviceHandlers.find(deviceKey);
        if (handlerIt != deviceHandlers.end())
        {
            auto* handler = handlerIt->second.get();
            std::lock_guard<std::mutex> handlerLock(handler->getClientBuffersMutex());

            auto& clientBuffers = handler->getClientBuffers();
            auto clientIt = clientBuffers.find(clientId);
            if (clientIt != clientBuffers.end() && clientIt->second.inputBuffer)
            {
                AudioClient::ChannelBufferRef ref;
                ref.subscription = sub;
                ref.buffer = clientIt->second.inputBuffer;
                ref.deviceChannelIndex = sub.channelIndex;
                newSnapshot->inputBuffers.push_back(std::move(ref));

                // Build group for realtime-safe access
                auto* syncBuf = clientIt->second.inputBuffer.get();
                auto& group = inputGroupMap[syncBuf];
                group.buffer = syncBuf;
                group.maxDeviceChannel = std::max(group.maxDeviceChannel, sub.channelIndex);
                group.channelMap.push_back({static_cast<int>(i), sub.channelIndex});
            }
        }
    }

    // Convert input group map to vector
    newSnapshot->inputGroups.reserve(inputGroupMap.size());
    for (auto& [ptr, group] : inputGroupMap)
        newSnapshot->inputGroups.push_back(std::move(group));

    // Build output buffer refs and group by SyncBuffer
    std::unordered_map<SyncBuffer*, AudioClient::BufferGroup> outputGroupMap;
    for (size_t i = 0; i < currentState.outputSubscriptions.size(); ++i)
    {
        const auto& sub = currentState.outputSubscriptions[i];
        juce::String deviceKey = resolveDeviceKeyForSubscription(sub);
        if (deviceKey.isEmpty())
            continue;

        auto handlerIt = deviceHandlers.find(deviceKey);
        if (handlerIt != deviceHandlers.end())
        {
            auto* handler = handlerIt->second.get();
            std::lock_guard<std::mutex> handlerLock(handler->getClientBuffersMutex());

            auto& clientBuffers = handler->getClientBuffers();
            auto clientIt = clientBuffers.find(clientId);
            if (clientIt != clientBuffers.end() && clientIt->second.outputBuffer)
            {
                AudioClient::ChannelBufferRef ref;
                ref.subscription = sub;
                ref.buffer = clientIt->second.outputBuffer;
                ref.deviceChannelIndex = sub.channelIndex;
                newSnapshot->outputBuffers.push_back(std::move(ref));

                // Build group for realtime-safe access
                auto* syncBuf = clientIt->second.outputBuffer.get();
                auto& group = outputGroupMap[syncBuf];
                group.buffer = syncBuf;
                group.maxDeviceChannel = std::max(group.maxDeviceChannel, sub.channelIndex);
                group.channelMap.push_back({static_cast<int>(i), sub.channelIndex});
            }
        }
    }

    // Convert output group map to vector
    newSnapshot->outputGroups.reserve(outputGroupMap.size());
    for (auto& [ptr, group] : outputGroupMap)
        newSnapshot->outputGroups.push_back(std::move(group));

    // Pre-allocate client temp buffers based on device channel counts
    // Server subscribes to ALL device channels; client-side ChannelRoutingMatrix filters
    int maxChannels = 0;
    for (const auto& [deviceKey, handler] : deviceHandlers)
        if (handler && handler->isDeviceOpen())
            maxChannels = std::max(maxChannels, handler->getNumChannels());

    if (maxChannels > 0)
    {
        // Use a reasonable default buffer size (will grow if needed, but this covers most cases)
        int defaultBufferSize = 2048;
        clientPtr->ensureTempBufferCapacity(maxChannels, defaultBufferSize);
    }

    // Atomically update client's buffer snapshot
    clientPtr->updateBufferSnapshot(std::move(newSnapshot));
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
        auto statePtr = it->second.state.load();
        return statePtr ? *statePtr : AudioClientState();
    }

    return AudioClientState();
}

juce::StringArray AudioServer::getAvailableInputDevices() const
{
    juce::StringArray devices;
    for (auto& entry : getInputDevicesByType())
        devices.addArray(entry.second);
    devices.removeDuplicates(false);
    return devices;
}

juce::StringArray AudioServer::getAvailableOutputDevices() const
{
    juce::StringArray devices;
    for (auto& entry : getOutputDevicesByType())
        devices.addArray(entry.second);
    devices.removeDuplicates(false);
    return devices;
}

std::map<juce::String, juce::StringArray> AudioServer::getInputDevicesByType() const
{
    if (!catalogReady.load(std::memory_order_acquire))
    {
        scanRequested.store(true, std::memory_order_release);
        const_cast<AudioServer*>(this)->startTimer(1);
    }
    std::lock_guard<std::mutex> lock(catalogMutex);
    return inputDeviceCatalog;
}

std::map<juce::String, juce::StringArray> AudioServer::getOutputDevicesByType() const
{
    if (!catalogReady.load(std::memory_order_acquire))
    {
        scanRequested.store(true, std::memory_order_release);
        const_cast<AudioServer*>(this)->startTimer(1);
    }
    std::lock_guard<std::mutex> lock(catalogMutex);
    return outputDeviceCatalog;
}

int AudioServer::getDeviceNumChannels(const juce::String& deviceName, bool isInput) const
{
    return getDeviceChannelNames(deviceName, isInput).size();
}

juce::StringArray AudioServer::getDeviceChannelNames(const juce::String& deviceName, bool isInput) const
{
    auto key = findDeviceKeyByName(deviceName);
    std::lock_guard<std::mutex> lock(deviceChannelCacheMutex);
    auto& cache = isInput ? inputDeviceChannelNamesCache : outputDeviceChannelNamesCache;
    auto found = cache.find(key);
    return found != cache.end() ? found->second : juce::StringArray();
}

juce::Array<double> AudioServer::getAvailableSampleRates(const juce::String& deviceName) const
{
    auto key = findDeviceKeyByName(deviceName);
    std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
    auto found = deviceSampleRatesCache.find(key);
    return found != deviceSampleRatesCache.end() ? found->second : juce::Array<double>();
}

juce::Array<int> AudioServer::getAvailableBufferSizes(const juce::String& deviceName) const
{
    auto key = findDeviceKeyByName(deviceName);
    std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
    auto found = deviceBufferSizesCache.find(key);
    return found != deviceBufferSizesCache.end() ? found->second : juce::Array<int>();
}

int AudioServer::getDefaultBufferSize(const juce::String& deviceName) const
{
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        juce::String deviceKey = findDeviceKeyByName(deviceName);
        auto it = deviceHandlers.find(deviceKey);
        if (it != deviceHandlers.end() && it->second->isDeviceOpen())
            return it->second->getBufferSize();
    }

    return 0;
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

    atk::logging::debug("AudioServer::cacheDeviceInfo", "cached capabilities for \"" + deviceName + "\"");
}

void AudioServer::invalidateDeviceCache(const juce::String& deviceName)
{
    atk::logging::debug("AudioServer::invalidateDeviceCache", "invalidating cache for \"" + deviceName + "\"");
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
        deviceSetupCache.erase(deviceName);
    }
}

void AudioServer::cacheDeviceSetup(
    const juce::String& deviceKey,
    const juce::AudioDeviceManager::AudioDeviceSetup& setup
)
{
    std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
    deviceSetupCache[deviceKey] = setup;
}

double AudioServer::getCurrentSampleRate(const juce::String& deviceName) const
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
    if (it != deviceHandlers.end() && it->second->isDeviceOpen())
        return it->second->getSampleRate();

    return 0.0;
}

int AudioServer::getCurrentBufferSize(const juce::String& deviceName) const
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
    if (it != deviceHandlers.end() && it->second->isDeviceOpen())
        return it->second->getBufferSize();

    return 0;
}

AudioDeviceHandler* AudioServer::getOrCreateDeviceHandler(const juce::String& deviceKey)
{
    auto it = deviceHandlers.find(deviceKey);
    if (it != deviceHandlers.end())
    {
        atk::logging::debug(
            "AudioServer::getOrCreateDeviceHandler",
            juce::String::formatted("reusing handler \"%s\"", deviceKey.toRawUTF8())
        );
        return it->second.get();
    }

    atk::logging::debug(
        "AudioServer::getOrCreateDeviceHandler",
        juce::String::formatted("creating handler \"%s\"", deviceKey.toRawUTF8())
    );
    // Parse device name from composite key (format: "deviceType|deviceName")
    int separatorIndex = deviceKey.indexOf("|");
    juce::String actualDeviceName = (separatorIndex >= 0) ? deviceKey.substring(separatorIndex + 1) : deviceKey;

    // Create new handler. Device is opened lazily on first subscription/direct callback,
    // and then kept until explicit server shutdown.
    auto handler = std::make_shared<AudioDeviceHandler>(actualDeviceName, deviceKey);
    auto* ptr = handler.get();
    deviceHandlers[deviceKey] = std::move(handler);

    return ptr;
}

bool AudioServer::registerDirectCallback(
    const juce::String& deviceName,
    juce::AudioIODeviceCallback* callback,
    const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup
)
{
    if (!initialized.load(std::memory_order_acquire))
    {
        atk::logging::warning("AudioServer::registerDirectCallback", "rejected: server not initialized");
        return false;
    }

    if (callback == nullptr)
    {
        atk::logging::warning("AudioServer::registerDirectCallback", "rejected: callback is null");
        return false;
    }

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    std::shared_ptr<AudioDeviceHandler> handler;
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        getOrCreateDeviceHandler(deviceKey);
        handler = deviceHandlers.at(deviceKey);
    }
    if (handler == nullptr)
    {
        atk::logging::warning(
            "AudioServer::registerDirectCallback",
            juce::String::formatted("failed to get handler for \"%s\"", deviceKey.toRawUTF8())
        );
        return false;
    }

    // Try to register the direct callback
    if (!handler->registerDirectCallback(callback))
        return false;

    if (!handler->isDeviceOpen()
        || (preferredSetup.sampleRate > 0.0 && preferredSetup.sampleRate != handler->getSampleRate())
        || (preferredSetup.bufferSize > 0 && preferredSetup.bufferSize != handler->getBufferSize()))
        handler->updatePreferredRecoverySetup(preferredSetup);
    if (!handler->isDeviceOpen())
        handler->markRecoveryPending();
    startTimer(1);
    atk::logging::info("AudioServer::registerDirectCallback", "registered direct callback for \"" + deviceName + "\"");
    return true;
}

void AudioServer::unregisterDirectCallback(const juce::String& deviceName, juce::AudioIODeviceCallback* callback)
{
    std::shared_ptr<AudioDeviceHandler> handler;
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        auto found = deviceHandlers.find(findDeviceKeyByName(deviceName));
        if (found != deviceHandlers.end())
            handler = found->second;
    }
    if (handler)
        handler->unregisterDirectCallback(callback);
    startTimer(1);
}

bool AudioServer::hasDirectCallback(const juce::String& deviceName) const
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
    if (it != deviceHandlers.end())
        return it->second->hasDirectCallback();

    return false;
}

bool AudioServer::setDeviceSampleRate(const juce::String& deviceName, double newSampleRate)
{
    if (newSampleRate <= 0.0)
        return false;
    auto availableRates = getAvailableSampleRates(deviceName);
    if (!availableRates.isEmpty() && !availableRates.contains(newSampleRate))
        return false;
    std::lock_guard<std::mutex> lock(devicesMutex);

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
    if (it == deviceHandlers.end())
    {
        atk::logging::warning("AudioServer::setDeviceSampleRate", "handler not found for \"" + deviceName + "\"");
        return false;
    }

    auto setup = it->second->getPreferredRecoverySetup();
    setup.sampleRate = newSampleRate;
    it->second->updatePreferredRecoverySetup(setup);
    it->second->markRecoveryPending();
    startTimer(1);
    return true;
}

bool AudioServer::setDeviceBufferSize(const juce::String& deviceName, int newBufferSize)
{
    if (newBufferSize <= 0)
        return false;
    auto availableSizes = getAvailableBufferSizes(deviceName);
    if (!availableSizes.isEmpty() && !availableSizes.contains(newBufferSize))
        return false;
    std::lock_guard<std::mutex> lock(devicesMutex);

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
    if (it == deviceHandlers.end())
    {
        atk::logging::warning("AudioServer::setDeviceBufferSize", "handler not found for \"" + deviceName + "\"");
        return false;
    }

    auto setup = it->second->getPreferredRecoverySetup();
    setup.bufferSize = newBufferSize;
    it->second->updatePreferredRecoverySetup(setup);
    it->second->markRecoveryPending();
    startTimer(1);
    return true;
}

bool AudioServer::getCurrentDeviceSetup(
    const juce::String& deviceName,
    juce::AudioDeviceManager::AudioDeviceSetup& outSetup
) const
{
    auto key = findDeviceKeyByName(deviceName);
    std::lock_guard<std::mutex> lock(deviceCapabilitiesCacheMutex);
    auto found = deviceSetupCache.find(key);
    if (found == deviceSetupCache.end())
        return false;
    outSetup = found->second;
    return true;
}

AudioDeviceHandler* AudioServer::getDeviceHandler(const juce::String& deviceName) const
{
    std::lock_guard<std::mutex> lock(devicesMutex);
    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
    if (it != deviceHandlers.end())
        return it->second.get();
    return nullptr;
}

} // namespace atk
