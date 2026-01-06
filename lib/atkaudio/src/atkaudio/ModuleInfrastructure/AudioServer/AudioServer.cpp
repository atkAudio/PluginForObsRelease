#include "AudioServer.h"
#include <atkaudio/atkaudio.h>

namespace atk
{

// AudioDeviceEnumerator statics
std::mutex AudioDeviceEnumerator::enumeratorMutex;
std::unique_ptr<juce::AudioDeviceManager> AudioDeviceEnumerator::enumerator;
std::mutex AudioDeviceEnumerator::cacheMutex;
std::unordered_map<juce::String, int> AudioDeviceEnumerator::inputChannelCache;
std::unordered_map<juce::String, int> AudioDeviceEnumerator::outputChannelCache;
std::unordered_map<juce::String, juce::StringArray> AudioDeviceEnumerator::inputChannelNamesCache;
std::unordered_map<juce::String, juce::StringArray> AudioDeviceEnumerator::outputChannelNamesCache;
std::unordered_map<juce::String, juce::Array<double>> AudioDeviceEnumerator::sampleRatesCache;
std::unordered_map<juce::String, juce::Array<int>> AudioDeviceEnumerator::bufferSizesCache;

juce::AudioDeviceManager* AudioDeviceEnumerator::ensureEnumerator()
{
    if (!enumerator)
    {
        std::lock_guard<std::mutex> lock(enumeratorMutex);
        if (!enumerator)
            enumerator = std::make_unique<juce::AudioDeviceManager>();
    }
    return enumerator.get();
}

juce::StringArray AudioDeviceEnumerator::getAvailableInputDevices()
{
    auto* mgr = ensureEnumerator();
    if (!mgr)
        return {};

    juce::StringArray devices;
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
        type->scanForDevices();
        devices.addArray(type->getDeviceNames(true));
    }
    devices.removeDuplicates(false);
    return devices;
}

juce::StringArray AudioDeviceEnumerator::getAvailableOutputDevices()
{
    auto* mgr = ensureEnumerator();
    if (!mgr)
        return {};

    juce::StringArray devices;
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
        type->scanForDevices();
        devices.addArray(type->getDeviceNames(false));
    }
    devices.removeDuplicates(false);
    return devices;
}

std::map<juce::String, juce::StringArray> AudioDeviceEnumerator::getInputDevicesByType()
{
    auto* mgr = ensureEnumerator();
    if (!mgr)
        return {};

    std::map<juce::String, juce::StringArray> result;
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
        type->scanForDevices();
        auto devices = type->getDeviceNames(true);
        if (devices.size() > 0)
            result[type->getTypeName()] = devices;
    }
    return result;
}

std::map<juce::String, juce::StringArray> AudioDeviceEnumerator::getOutputDevicesByType()
{
    auto* mgr = ensureEnumerator();
    if (!mgr)
        return {};

    std::map<juce::String, juce::StringArray> result;
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
        type->scanForDevices();
        auto devices = type->getDeviceNames(false);
        if (devices.size() > 0)
            result[type->getTypeName()] = devices;
    }
    return result;
}

int AudioDeviceEnumerator::getDeviceNumChannels(const juce::String& deviceName, bool isInput)
{
    // Check cache
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto& cache = isInput ? inputChannelCache : outputChannelCache;
        auto it = cache.find(deviceName);
        if (it != cache.end())
            return it->second;
    }

    auto* mgr = ensureEnumerator();
    if (!mgr)
        return 0;

    int numChannels = 0;
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
        type->scanForDevices();
        auto inputDevs = type->getDeviceNames(true);
        auto outputDevs = type->getDeviceNames(false);

        if (inputDevs.contains(deviceName) || outputDevs.contains(deviceName))
        {
            std::unique_ptr<juce::AudioIODevice> device(type->createDevice(deviceName, deviceName));
            if (device)
            {
                auto inputNames = device->getInputChannelNames();
                auto outputNames = device->getOutputChannelNames();

                // Cache both
                {
                    std::lock_guard<std::mutex> lock(cacheMutex);
                    inputChannelCache[deviceName] = inputNames.size();
                    outputChannelCache[deviceName] = outputNames.size();
                    inputChannelNamesCache[deviceName] = inputNames;
                    outputChannelNamesCache[deviceName] = outputNames;
                }

                numChannels = isInput ? inputNames.size() : outputNames.size();
                break;
            }
        }
    }
    return numChannels;
}

juce::StringArray AudioDeviceEnumerator::getDeviceChannelNames(const juce::String& deviceName, bool isInput)
{
    // Check cache
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto& cache = isInput ? inputChannelNamesCache : outputChannelNamesCache;
        auto it = cache.find(deviceName);
        if (it != cache.end())
            return it->second;
    }

    // Populate cache via getDeviceNumChannels
    getDeviceNumChannels(deviceName, isInput);

    // Return from cache
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto& cache = isInput ? inputChannelNamesCache : outputChannelNamesCache;
        auto it = cache.find(deviceName);
        if (it != cache.end())
            return it->second;
    }
    return {};
}

juce::Array<double> AudioDeviceEnumerator::getAvailableSampleRates(const juce::String& deviceName)
{
    // Check cache
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = sampleRatesCache.find(deviceName);
        if (it != sampleRatesCache.end())
            return it->second;
    }

    auto* mgr = ensureEnumerator();
    if (!mgr)
        return {};

    juce::Array<double> rates;
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
        auto devs = type->getDeviceNames(false);
        if (devs.contains(deviceName))
        {
            std::unique_ptr<juce::AudioIODevice> device(type->createDevice(deviceName, deviceName));
            if (device)
            {
                rates = device->getAvailableSampleRates();
                std::lock_guard<std::mutex> lock(cacheMutex);
                sampleRatesCache[deviceName] = rates;
                break;
            }
        }
    }
    return rates;
}

juce::Array<int> AudioDeviceEnumerator::getAvailableBufferSizes(const juce::String& deviceName)
{
    // Check cache
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = bufferSizesCache.find(deviceName);
        if (it != bufferSizesCache.end())
            return it->second;
    }

    auto* mgr = ensureEnumerator();
    if (!mgr)
        return {};

    juce::Array<int> sizes;
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
        auto devs = type->getDeviceNames(false);
        if (devs.contains(deviceName))
        {
            std::unique_ptr<juce::AudioIODevice> device(type->createDevice(deviceName, deviceName));
            if (device)
            {
                sizes = device->getAvailableBufferSizes();
                std::lock_guard<std::mutex> lock(cacheMutex);
                bufferSizesCache[deviceName] = sizes;
                break;
            }
        }
    }
    return sizes;
}

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

        if (group.buffer->read(tempInputPointers.data(), numDevCh, numSamples, sampleRate, false))
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
        // Ensure callback is registered (safe to call multiple times)
        deviceManager->addAudioCallback(this);
        return true;
    }

    // Add callback BEFORE opening device (important!)
    deviceManager->addAudioCallback(this);

    // Initialize device manager to make device types available
    deviceManager->initialiseWithDefaultDevices(0, 0);

    // Find the device type
    juce::AudioIODeviceType* deviceType = nullptr;

    for (auto* type : deviceManager->getAvailableDeviceTypes())
    {
        auto inputDevices = type->getDeviceNames(true);
        auto outputDevices = type->getDeviceNames(false);

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

    // Set the current device type in the manager
    deviceManager->setCurrentAudioDeviceType(deviceType->getTypeName(), true);

    // Create setup
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.sampleRate = preferredSetup.sampleRate;
    setup.bufferSize = preferredSetup.bufferSize;

    // Device names may differ between input and output lists
    auto inputDevices = deviceType->getDeviceNames(true);
    auto outputDevices = deviceType->getDeviceNames(false);
    bool deviceIsInput = inputDevices.contains(deviceName);
    bool deviceIsOutput = outputDevices.contains(deviceName);

    setup.inputDeviceName = deviceIsInput ? deviceName : juce::String();
    setup.outputDeviceName = deviceIsOutput ? deviceName : juce::String();

    // Must explicitly enable channels for the device to start playing
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;

    // Copy channel configuration from preferredSetup - if they're zero, enable all
    setup.inputChannels = preferredSetup.inputChannels;
    setup.outputChannels = preferredSetup.outputChannels;

    if (setup.inputChannels.isZero() && setup.outputChannels.isZero())
    {
        // No specific channels - enable all available
        setup.inputChannels.setRange(0, 256, true);
        setup.outputChannels.setRange(0, 256, true); // JUCE will limit to actual count
    }

    // Apply the setup
    juce::String error = deviceManager->setAudioDeviceSetup(setup, true);

    if (error.isEmpty())
    {
        auto* device = deviceManager->getCurrentAudioDevice();

        // Populate cache with actual device channel info
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
            deviceManager->restartLastAudioDevice();

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
    auto directSnapshot = directCallbackSnapshot.load(std::memory_order_acquire);
    if (directSnapshot && !directSnapshot->callbacks.empty())
    {
        for (DirectCallbackInfo* info : directSnapshot->callbacks)
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

// AudioServer

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

void AudioServer::setupDeviceEnumeratorListeners()
{
    if (!deviceEnumerator)
        return;

    deviceEnumerator->addChangeListener(const_cast<AudioServer*>(this));
}

void AudioServer::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == deviceEnumerator.get())
        listeners.call([](Listener& l) { l.audioServerDeviceListChanged(); });
}

void AudioServer::addListener(Listener* listener)
{
    listeners.add(listener);
}

void AudioServer::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

void AudioServer::initialize()
{
    if (initialized.load(std::memory_order_acquire))
        return;

    DBG("AudioServer: Initializing...");

    ensureDeviceEnumerator();
    setupDeviceEnumeratorListeners();

    initialized.store(true, std::memory_order_release);

    DBG("AudioServer: Initialized");
}

void AudioServer::shutdown()
{
    if (!initialized.load(std::memory_order_acquire))
        return;

    initialized.store(false, std::memory_order_release);

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

    // Clear device type cache
    {
        std::lock_guard<std::mutex> lock(deviceTypeCacheMutex);
        deviceNameToTypeCache.clear();
    }

    deviceEnumerator.reset();
}

juce::String AudioServer::makeDeviceKey(const juce::String& deviceType, const juce::String& deviceName)
{
    return deviceType + "|" + deviceName;
}

juce::String AudioServer::makeDeviceKey(const ChannelSubscription& sub)
{
    return makeDeviceKey(sub.deviceType, sub.deviceName);
}

juce::String AudioServer::findDeviceKeyByName(const juce::String& deviceName) const
{
    // First check if we already have a handler with this device name
    for (const auto& [key, handler] : deviceHandlers)
    {
        int separatorIndex = key.indexOf("|");
        juce::String keyDeviceName = (separatorIndex >= 0) ? key.substring(separatorIndex + 1) : key;
        if (keyDeviceName == deviceName)
            return key;
    }

    // Check device type cache (avoids slow scanForDevices)
    {
        std::lock_guard<std::mutex> lock(deviceTypeCacheMutex);
        auto it = deviceNameToTypeCache.find(deviceName);
        if (it != deviceNameToTypeCache.end())
            return makeDeviceKey(it->second, deviceName);
    }

    // Not in cache - discover device type from device enumerator (slow path)
    auto* enumerator = ensureDeviceEnumerator();
    if (enumerator)
    {
        for (auto* type : enumerator->getAvailableDeviceTypes())
        {
            type->scanForDevices();
            auto inputDevices = type->getDeviceNames(true);
            auto outputDevices = type->getDeviceNames(false);

            // Cache ALL discovered devices from this type while we're scanning
            {
                std::lock_guard<std::mutex> lock(deviceTypeCacheMutex);
                for (const auto& dev : inputDevices)
                    deviceNameToTypeCache[dev] = type->getTypeName();
                for (const auto& dev : outputDevices)
                    deviceNameToTypeCache[dev] = type->getTypeName();
            }

            if (inputDevices.contains(deviceName) || outputDevices.contains(deviceName))
                return makeDeviceKey(type->getTypeName(), deviceName);
        }
    }

    // Fallback: use deviceName as key (legacy compatibility)
    return deviceName;
}

void AudioServer::processDeviceCleanup()
{
    // Called during subscription updates to process deferred device closures
    // Must be called with devicesMutex held
    juce::int64 currentTime = juce::Time::currentTimeMillis();

    auto it = pendingDeviceCloses.begin();
    while (it != pendingDeviceCloses.end())
    {
        if (currentTime >= it->closeTime)
        {
            // Check if device still has no connections
            auto handlerIt = deviceHandlers.find(it->deviceKey);
            if (handlerIt != deviceHandlers.end())
            {
                if (!handlerIt->second->hasDirectCallback() && !handlerIt->second->hasActiveSubscriptions())
                {
                    DBG("AudioServer: Closing device '" + it->deviceKey + "' after deferred timeout");
                    deviceHandlers.erase(handlerIt);
                }
            }
            it = pendingDeviceCloses.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool AudioServer::cancelPendingDeviceClose(const juce::String& deviceKey)
{
    // Must be called with devicesMutex held
    auto it = std::find_if(
        pendingDeviceCloses.begin(),
        pendingDeviceCloses.end(),
        [&deviceKey](const PendingDeviceClose& pending) { return pending.deviceKey == deviceKey; }
    );

    if (it != pendingDeviceCloses.end())
    {
        DBG("AudioServer: Cancelled pending close for device '" + deviceKey + "'");
        pendingDeviceCloses.erase(it);
        return true;
    }
    return false;
}

void AudioServer::scheduleDeviceClose(const juce::String& deviceKey)
{
    // Must be called with devicesMutex held
    PendingDeviceClose pending;
    pending.deviceKey = deviceKey;
    pending.closeTime = juce::Time::currentTimeMillis() + DEVICE_CLOSE_DELAY_MS;

    pendingDeviceCloses.push_back(pending);
    DBG("AudioServer: Scheduled device '" + deviceKey + "' for deferred close");
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
    {
        std::lock_guard<std::mutex> lock(clientsMutex);

        auto it = clients.find(clientId);
        if (it == clients.end())
            return;

        auto currentState = it->second.state.load();
        if (currentState && *currentState == state)
        {
            DBG("AudioServer: Subscriptions unchanged - skipping update ("
                << state.inputSubscriptions.size()
                << " in, "
                << state.outputSubscriptions.size()
                << " out)");
            return;
        }

        // Update state atomically (old shared_ptr deleted automatically when refcount hits 0)
        it->second.state.store(std::make_shared<AudioClientState>(state));
    }

    DBG("AudioServer: Subscriptions changed - applying update ("
        << state.inputSubscriptions.size()
        << " in, "
        << state.outputSubscriptions.size()
        << " out)");

    // Update device handlers (acquire devicesMutex AFTER releasing clientsMutex to avoid deadlock)
    std::lock_guard<std::mutex> deviceLock(devicesMutex);

    // Process any pending device cleanup first
    processDeviceCleanup();

    // ATOMIC SWAP APPROACH: Build new subscription state, then swap atomically
    // Use composite device keys (deviceType|deviceName) to avoid conflicts

    // Step 1: Group NEW subscriptions by device key
    std::unordered_map<juce::String, std::vector<ChannelSubscription>> newInputSubs;
    std::unordered_map<juce::String, std::vector<ChannelSubscription>> newOutputSubs;

    for (const auto& sub : state.inputSubscriptions)
        newInputSubs[makeDeviceKey(sub)].push_back(sub);

    for (const auto& sub : state.outputSubscriptions)
        newOutputSubs[makeDeviceKey(sub)].push_back(sub);

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
            std::lock_guard<std::mutex> handlerLock(handler->clientBuffersMutex);
            if (handler->clientBuffers.find(clientId) != handler->clientBuffers.end())
                allDeviceKeys.insert(key);
        }
    }

    // Step 3: For each device, atomically update subscriptions
    for (const auto& deviceKey : allDeviceKeys)
    {
        // Cancel any pending close for this device (it's being reused)
        cancelPendingDeviceClose(deviceKey);

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

            // Add new input subscriptions
            if (!newInput.empty())
            {
                bool justOpened = false;
                if (!handler->isDeviceOpen())
                {
                    juce::AudioDeviceManager::AudioDeviceSetup setup;
                    setup.sampleRate = 0.0;
                    setup.bufferSize = 0;

                    handlerLock.unlock();
                    bool opened = handler->openDevice(setup);
                    handlerLock.lock();

                    if (!opened)
                        continue;

                    justOpened = true;
                }

                auto& buffers = handler->clientBuffers[clientId];

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
                {
                    buffers.inputBuffer = std::make_shared<SyncBuffer>();

                    int numChannels = 2;
                    if (auto* device = handler->deviceManager->getCurrentAudioDevice())
                        numChannels = device->getActiveInputChannels().countNumberOfSetBits();

                    juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
                    dummyBuffer.clear();
                    std::vector<float*> dummyPointers(numChannels);
                    for (int ch = 0; ch < numChannels; ++ch)
                        dummyPointers[ch] = dummyBuffer.getWritePointer(ch);

                    buffers.inputBuffer->read(dummyPointers.data(), numChannels, 480, 48000.0, false);
                }

                snapshotDirty = true;

                if (justOpened || !handler->isRunning.load(std::memory_order_acquire))
                    handler->isRunning.store(true, std::memory_order_release);
            }

            // Add new output subscriptions
            if (!newOutput.empty())
            {
                bool justOpened = false;
                if (!handler->isDeviceOpen())
                {
                    juce::AudioDeviceManager::AudioDeviceSetup setup;
                    setup.sampleRate = 0.0;
                    setup.bufferSize = 0;

                    handlerLock.unlock();
                    bool opened = handler->openDevice(setup);
                    handlerLock.lock();

                    if (!opened)
                        continue;

                    justOpened = true;
                }

                auto& buffers = handler->clientBuffers[clientId];

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
                {
                    buffers.outputBuffer = std::make_shared<SyncBuffer>();

                    int numChannels = 2;
                    if (auto* device = handler->deviceManager->getCurrentAudioDevice())
                        numChannels = device->getActiveOutputChannels().countNumberOfSetBits();

                    juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
                    dummyBuffer.clear();
                    std::vector<const float*> dummyPointers(numChannels);
                    for (int ch = 0; ch < numChannels; ++ch)
                        dummyPointers[ch] = dummyBuffer.getReadPointer(ch);

                    buffers.outputBuffer->write(dummyPointers.data(), numChannels, 480, 48000.0);
                }

                snapshotDirty = true;

                if (justOpened || !handler->isRunning.load(std::memory_order_acquire))
                    handler->isRunning.store(true, std::memory_order_release);
            }

            if (snapshotDirty)
                handler->rebuildSnapshotLocked();
        }

        // Close device if no more subscriptions
        if (!handler->hasActiveSubscriptions() && handler->isDeviceOpen())
            handler->closeDevice();
    }

    // Step 4: Clean up unused device handlers (schedule for deferred close)
    for (auto& [key, handler] : deviceHandlers)
        if (!handler->hasActiveSubscriptions() && !handler->hasDirectCallback())
            scheduleDeviceClose(key);

    // Step 5: Rebuild client's buffer snapshot for lock-free audio access
    rebuildClientBufferSnapshot(clientId);

    DBG("AudioServer: Updated subscriptions for client "
        + juce::String::toHexString((juce::pointer_sized_int)clientId));
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
        juce::String deviceKey = makeDeviceKey(sub);

        auto handlerIt = deviceHandlers.find(deviceKey);
        if (handlerIt != deviceHandlers.end())
        {
            auto* handler = handlerIt->second.get();
            std::lock_guard<std::mutex> handlerLock(handler->clientBuffersMutex);

            auto clientIt = handler->clientBuffers.find(clientId);
            if (clientIt != handler->clientBuffers.end() && clientIt->second.inputBuffer)
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
        juce::String deviceKey = makeDeviceKey(sub);

        auto handlerIt = deviceHandlers.find(deviceKey);
        if (handlerIt != deviceHandlers.end())
        {
            auto* handler = handlerIt->second.get();
            std::lock_guard<std::mutex> handlerLock(handler->clientBuffersMutex);

            auto clientIt = handler->clientBuffers.find(clientId);
            if (clientIt != handler->clientBuffers.end() && clientIt->second.outputBuffer)
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
        juce::String deviceKey = findDeviceKeyByName(deviceName);
        auto it = deviceHandlers.find(deviceKey);
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
        DBG("AudioServer: Reusing existing device handler for '" + deviceKey + "'");
        return it->second.get();
    }

    DBG("AudioServer: Creating NEW device handler for '" + deviceKey + "'");

    // Parse device name from composite key (format: "deviceType|deviceName")
    int separatorIndex = deviceKey.indexOf("|");
    juce::String actualDeviceName = (separatorIndex >= 0) ? deviceKey.substring(separatorIndex + 1) : deviceKey;

    // Create new handler (does NOT open the device yet - will open on first subscription)
    auto handler = std::make_unique<AudioDeviceHandler>(actualDeviceName);
    auto* ptr = handler.get();
    deviceHandlers[deviceKey] = std::move(handler);

    return ptr;
}

void AudioServer::removeDeviceHandlerIfUnused(const juce::String& deviceKey)
{
    auto it = deviceHandlers.find(deviceKey);
    if (it != deviceHandlers.end() && !it->second->hasActiveSubscriptions())
    {
        DBG("AudioServer: Closing unused device handler for key '" + deviceKey + "'");
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

    // Find or create device key from device name
    juce::String deviceKey = findDeviceKeyByName(deviceName);

    // Get or create device handler
    auto* handler = getOrCreateDeviceHandler(deviceKey);
    if (handler == nullptr)
    {
        DBG("AudioServer: Failed to create device handler for '" + deviceName + "'");
        return false;
    }

    // Check if there was a pending close - this indicates device went away and came back
    // In that case, we need to force a full reopen to get a fresh connection
    bool hadPendingClose = cancelPendingDeviceClose(deviceKey);
    if (hadPendingClose)
    {
        DBG("[Hotplug] Device had pending close - forcing full reopen for '" + deviceName + "'");
        handler->closeDevice();
    }

    // Try to register the direct callback
    if (!handler->registerDirectCallback(callback))
        return false;

    // Open device if not already open
    // Check if device needs to be reopened due to parameter changes
    bool needsReopen = false;
    if (handler->isDeviceOpen())
    {
        auto* device = handler->deviceManager->getCurrentAudioDevice();
        if (device == nullptr)
        {
            // Handler thinks it's open but device is gone (e.g., after hotplug)
            DBG("[Hotplug] Device handler open but no device - forcing reopen");
            needsReopen = true;
        }
        else if (!device->isOpen())
        {
            // Device exists but isn't actually open
            DBG("[Hotplug] Device exists but not open - forcing reopen");
            needsReopen = true;
        }
        else if (hadPendingClose && !device->isPlaying())
        {
            // Only check isPlaying after actual hotplug (hadPendingClose)
            // During normal config changes, not playing is expected
            DBG("[Hotplug] Device exists but not playing after hotplug - forcing reopen");
            needsReopen = true;
        }
        else
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

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
    if (it != deviceHandlers.end())
    {
        it->second->unregisterDirectCallback(callback);

        // Schedule deferred close if no longer needed
        if (!it->second->hasDirectCallback() && !it->second->hasActiveSubscriptions())
            scheduleDeviceClose(deviceKey);
    }
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
    std::lock_guard<std::mutex> lock(devicesMutex);

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
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

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
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

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
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
    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
    if (it != deviceHandlers.end())
        return it->second.get();
    return nullptr;
}

} // namespace atk
