#include "AudioServer.h"
#include <atkaudio/atkaudio.h>
#include <atkaudio/GlobalSettings.h>
#include <atkaudio/Logging.h>

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

void AudioDeviceEnumerator::shutdown()
{
    // Must run before static destruction: ~AudioDeviceManager touches JUCE singletons that
    // are already gone by the time a static unique_ptr is finalised.
    {
        std::lock_guard<std::mutex> lock(enumeratorMutex);
        enumerator.reset();
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    inputChannelCache.clear();
    outputChannelCache.clear();
    inputChannelNamesCache.clear();
    outputChannelNamesCache.clear();
    sampleRatesCache.clear();
    bufferSizesCache.clear();
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

    // getAvailableDeviceTypes() calls scanDevicesIfNeeded() which performs the first scan once.
    // Do not call type->scanForDevices() here — redundant rescans cause ~100 ms stalls.
    std::map<juce::String, juce::StringArray> result;
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
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

    // getAvailableDeviceTypes() calls scanDevicesIfNeeded() which performs the first scan once.
    // Do not call type->scanForDevices() here — redundant rescans cause ~100 ms stalls.
    std::map<juce::String, juce::StringArray> result;
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
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
        if (devs.contains(deviceName) || type->getDeviceNames(true).contains(deviceName))
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

bool AudioDeviceEnumerator::getCurrentHardwareSetup(const juce::String& deviceName, double& sampleRate, int& bufferSize)
{
    auto* mgr = ensureEnumerator();
    if (!mgr)
        return false;

    // Deliberately uncached: these are live hardware values that the user can change at any time.
    for (auto* type : mgr->getAvailableDeviceTypes())
    {
        bool isOutput = type->getDeviceNames(false).contains(deviceName);
        bool isInput = type->getDeviceNames(true).contains(deviceName);

        if (isOutput || isInput)
        {
            std::unique_ptr<juce::AudioIODevice> device(
                type->createDevice(isOutput ? deviceName : juce::String(), isInput ? deviceName : juce::String())
            );
            if (device)
            {
                sampleRate = device->getCurrentSampleRate();
                bufferSize = device->getCurrentBufferSizeSamples();

                if ((sampleRate <= 0.0 || bufferSize <= 0) && device->getTypeName().startsWith("Windows Audio"))
                {
                    juce::BigInteger inputChannels;
                    juce::BigInteger outputChannels;
                    if (isInput)
                        inputChannels.setRange(0, 256, true);
                    if (isOutput)
                        outputChannels.setRange(0, 256, true);

                    auto openError = device->open(inputChannels, outputChannels, 0.0, 0);
                    if (openError.isEmpty())
                    {
                        sampleRate = device->getCurrentSampleRate();
                        bufferSize = device->getCurrentBufferSizeSamples();
                        device->close();
                    }
                }

                // ASIO only assigns its block size while open, so ask the driver what it prefers.
                if (bufferSize <= 0)
                    bufferSize = device->getDefaultBufferSize();

                return sampleRate > 0.0 && bufferSize > 0;
            }
        }
    }

    return false;
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

bool AudioDeviceHandler::openDevice(const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup)
{
    updatePreferredRecoverySetup(preferredSetup);

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

    auto* enumerator = AudioDeviceEnumerator::ensureEnumerator();
    if (!enumerator)
    {
        atk::logging::warning("AudioDeviceHandler::openDevice", "device enumerator not available");
        markRecoveryPending();
        return false;
    }

    // Find device type that has this device
    juce::AudioIODeviceType* deviceType = nullptr;
    for (auto* type : enumerator->getAvailableDeviceTypes())
    {
        if (type->getDeviceNames(true).contains(deviceName) || type->getDeviceNames(false).contains(deviceName))
        {
            deviceType = type;
            break;
        }
    }

    if (deviceType == nullptr)
    {
        atk::logging::warning("AudioDeviceHandler::openDevice", "device type not found for \"" + deviceName + "\"");
        markRecoveryPending();
        return false;
    }

    // Check that device exists in the selected type
    auto inputDevices = deviceType->getDeviceNames(true);
    auto outputDevices = deviceType->getDeviceNames(false);
    bool isInput = inputDevices.contains(deviceName);
    bool isOutput = outputDevices.contains(deviceName);

    if (!isInput && !isOutput)
    {
        atk::logging::warning("AudioDeviceHandler::openDevice", "device not found for \"" + deviceName + "\"");
        markRecoveryPending();
        return false;
    }

    // Get sample rate and buffer size from explicit setup or current hardware.
    auto options = getPreferredRecoverySetup();

    if (options.sampleRate <= 0.0 || options.bufferSize <= 0)
    {
        double hardwareSampleRate = 0.0;
        int hardwareBufferSize = 0;
        if (AudioDeviceEnumerator::getCurrentHardwareSetup(deviceName, hardwareSampleRate, hardwareBufferSize))
        {
            if (options.sampleRate <= 0.0 && hardwareSampleRate > 0.0)
                options.sampleRate = hardwareSampleRate;

            if (options.bufferSize <= 0 && hardwareBufferSize > 0)
                options.bufferSize = hardwareBufferSize;

            atk::logging::debug(
                "AudioDeviceHandler::openDevice",
                juce::String::formatted(
                    "current hardware setup for \"%s\": %.2f Hz / %d samples",
                    deviceName.toRawUTF8(),
                    hardwareSampleRate,
                    hardwareBufferSize
                )
            );
        }
        else
        {
            atk::logging::debug(
                "AudioDeviceHandler::openDevice",
                "current hardware setup unavailable for \"" + deviceName + "\""
            );
        }
    }

    if (options.sampleRate <= 0.0 || options.bufferSize <= 0)
    {
        atk::logging::warning(
            "AudioDeviceHandler::openDevice",
            juce::String::formatted(
                "refusing to open device \"%s\" without explicit sample rate and buffer size: %.2f Hz / %d samples",
                deviceName.toRawUTF8(),
                options.sampleRate,
                options.bufferSize
            )
        );
        markRecoveryPending();
        return false;
    }

    // Create the device directly (bypassing AudioDeviceManager)
    device = std::unique_ptr<juce::AudioIODevice>(
        deviceType->createDevice(isOutput ? deviceName : juce::String(), isInput ? deviceName : juce::String())
    );

    if (!device)
    {
        atk::logging::warning("AudioDeviceHandler::openDevice", "failed to create device \"" + deviceName + "\"");
        markRecoveryPending();
        return false;
    }

    // Prepare channel configuration
    juce::BigInteger inputChannels;
    juce::BigInteger outputChannels;
    if (isInput)
        inputChannels.setRange(0, 256, true);
    if (isOutput)
        outputChannels.setRange(0, 256, true);

    // Open the device
    juce::String error = device->open(inputChannels, outputChannels, options.sampleRate, options.bufferSize);

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
            deviceName,
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
    reportNegotiatedSetup("open");
    clearRecoveryPending();

    return true;
}

void AudioDeviceHandler::closeDevice()
{
    if (!isDeviceOpen())
        return;

    device->stop();
    device->close();
    device.reset();
}

bool AudioDeviceHandler::isDeviceOpen() const
{
    return device != nullptr && device->isOpen();
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
        for (const auto& info : directSnapshot->callbacks)
        {
            if (!info || info->callback == nullptr)
                continue;

            // Skip if temp buffer not sized correctly
            if (numOutputChannels > info->tempOutputBuffer.getNumChannels())
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

    rtSubscriptionTempBuffer.setSize(maxChannels, bufferSize, false, false, true);
    rtSubscriptionPointers.resize(maxChannels);
    rtInputPointers.resize(maxChannels);

    {
        std::lock_guard<std::mutex> lock(directCallbackMutex);
        for (auto& [callback, info] : directCallbacks)
            prepareDirectCallback(*info, *device);
        rebuildDirectCallbackSnapshotLocked();
    }

    auto directSnapshot = directCallbackSnapshot.load(std::memory_order_acquire);
    if (directSnapshot)
        for (const auto& info : directSnapshot->callbacks)
            if (info && info->callback != nullptr)
                info->callback->audioDeviceAboutToStart(device);

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
        server->invalidateDeviceCache(deviceName);
        server->cacheDeviceInfo(
            deviceName,
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

        if (!openDevice(setup))
        {
            atk::logging::warning(
                "AudioDeviceHandler::addClientSubscription",
                "failed to open device \"" + deviceName + "\""
            );
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
            buffers.inputBuffer = std::make_shared<SyncBuffer>(deviceName + " in");

            // Use device's actual input channel count
            int numChannels = 2; // Default fallback
            if (device)
                numChannels = device->getActiveInputChannels().countNumberOfSetBits();

            // Pre-configure reader side with OBS parameters (typical: 48kHz, 480 samples)
            // This allows writer (device callback) to prepare immediately
            juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
            dummyBuffer.clear();
            std::vector<float*> dummyPointers(numChannels);
            for (int ch = 0; ch < numChannels; ++ch)
                dummyPointers[ch] = dummyBuffer.getWritePointer(ch);

            buffers.inputBuffer->read(dummyPointers.data(), numChannels, 480, 48000.0, false);

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

            // Use device's actual output channel count
            int numChannels = 2; // Default fallback
            if (device)
                numChannels = device->getActiveOutputChannels().countNumberOfSetBits();

            // Pre-configure writer side with OBS parameters (client will write at 48kHz, 480
            // samples) This allows reader (device callback) to prepare immediately
            juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
            dummyBuffer.clear();
            std::vector<const float*> dummyPointers(numChannels);
            for (int ch = 0; ch < numChannels; ++ch)
                dummyPointers[ch] = dummyBuffer.getReadPointer(ch);

            buffers.outputBuffer->write(dummyPointers.data(), numChannels, 480, 48000.0);

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

        if (device != nullptr && device->isPlaying())
            prepareDirectCallback(*info, *device);

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
        atk::logging::debug(
            "AudioDeviceHandler::unregisterDirectCallback",
            "removed direct callback for device \"" + deviceName + "\""
        );
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
        newSnapshot->callbacks.push_back(info);

    // Atomic publish - audio callback can now see new snapshot
    directCallbackSnapshot.store(newSnapshot, std::memory_order_release);
}

void AudioDeviceHandler::updatePreferredRecoverySetup(const juce::AudioDeviceManager::AudioDeviceSetup& preferredSetup)
{
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
    if (device)
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
    if (device)
        return device->getCurrentSampleRate();

    // Return 0.0 to indicate no device is open (caller should check)
    return 0.0;
}

int AudioDeviceHandler::getBufferSize() const
{
    if (device)
        return device->getCurrentBufferSizeSamples();

    // Return 0 to indicate no device is open (caller should check)
    return 0;
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
    // Double-checked locking for thread-safe lazy initialization
    if (!deviceEnumerator)
    {
        std::lock_guard<std::mutex> lock(deviceEnumeratorMutex);
        if (!deviceEnumerator)
        {
            atk::logging::debug("AudioServer::ensureDeviceEnumerator", "creating device enumerator");
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
    {
        listeners.call([](Listener& l) { l.audioServerDeviceListChanged(); });
        attemptPendingDeviceRecovery("device_list_changed");
    }
}

void AudioServer::timerCallback()
{
    attemptPendingDeviceRecovery("periodic_retry");
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

    atk::logging::info("AudioServer::initialize", "begin");
    ensureDeviceEnumerator();
    setupDeviceEnumeratorListeners();

    initialized.store(true, std::memory_order_release);
    startTimer(recoveryRetryIntervalMs);

    atk::logging::info("AudioServer::initialize", "completed");
}

void AudioServer::shutdown()
{
    if (!initialized.load(std::memory_order_acquire))
        return;

    atk::logging::info("AudioServer::shutdown", "begin");

    initialized.store(false, std::memory_order_release);
    stopTimer();

    // Close all device handlers
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
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
    AudioDeviceEnumerator::shutdown();

    atk::logging::info("AudioServer::shutdown", "completed");
}

bool AudioServer::shouldRecoverDevice(AudioDeviceHandler* handler) const
{
    if (handler == nullptr)
        return false;

    if (!handler->hasActiveSubscriptions() && !handler->hasDesiredSubscriptions())
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

    bool recoveredAnyDevice = false;

    {
        std::lock_guard<std::mutex> lock(devicesMutex);

        double nowMs = juce::Time::getMillisecondCounterHiRes();

        for (auto& [deviceKey, handler] : deviceHandlers)
        {
            if (!shouldRecoverDevice(handler.get()))
                continue;

            if (!handler->canAttemptRecovery(nowMs, recoveryRetryIntervalMs))
                continue;

            auto setup = handler->getPreferredRecoverySetup();

            atk::logging::debug(
                "AudioServer::attemptPendingDeviceRecovery",
                "attempting recovery for \"" + deviceKey + "\" reason=" + reason
            );

            if (handler->isDeviceOpen())
                handler->closeDevice();

            if (handler->openDevice(setup))
            {
                handler->resetSubscriptionBuffersAfterRecovery();
                recoveredAnyDevice = true;
                atk::logging::info(
                    "AudioServer::attemptPendingDeviceRecovery",
                    "recovered device \"" + deviceKey + "\""
                );
            }
        }
    }

    if (!recoveredAnyDevice)
        return;

    std::vector<std::pair<void*, AudioClientState>> activeClientStates;
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        activeClientStates.reserve(clients.size());

        for (auto& [clientId, info] : clients)
        {
            auto statePtr = info.state.load();
            if (!statePtr)
                continue;

            activeClientStates.push_back({clientId, *statePtr});
        }
    }

    for (auto& [clientId, state] : activeClientStates)
        updateClientSubscriptions(clientId, state);
}

juce::StringArray AudioServer::getOpenDeviceNames() const
{
    std::lock_guard<std::mutex> lock(devicesMutex);
    juce::StringArray names;
    for (const auto& [key, handler] : deviceHandlers)
    {
        int sep = key.indexOf("|");
        juce::String deviceName = (sep >= 0) ? key.substring(sep + 1) : key;
        if (deviceName.isNotEmpty())
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

    return findDeviceKeyByName(sub.deviceName);
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
                {
                    buffers.inputBuffer = std::make_shared<SyncBuffer>(handler->getDeviceName() + " in");

                    int numChannels = 2;
                    if (auto* device = handler->getCurrentDevice())
                        numChannels = device->getActiveInputChannels().countNumberOfSetBits();

                    juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
                    dummyBuffer.clear();
                    std::vector<float*> dummyPointers(numChannels);
                    for (int ch = 0; ch < numChannels; ++ch)
                        dummyPointers[ch] = dummyBuffer.getWritePointer(ch);

                    buffers.inputBuffer->read(dummyPointers.data(), numChannels, 480, 48000.0, false);
                }

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
                {
                    buffers.outputBuffer = std::make_shared<SyncBuffer>(handler->getDeviceName() + " out");

                    int numChannels = 2;
                    if (auto* device = handler->getCurrentDevice())
                        numChannels = device->getActiveOutputChannels().countNumberOfSetBits();

                    juce::AudioBuffer<float> dummyBuffer(numChannels, 480);
                    dummyBuffer.clear();
                    std::vector<const float*> dummyPointers(numChannels);
                    for (int ch = 0; ch < numChannels; ++ch)
                        dummyPointers[ch] = dummyBuffer.getReadPointer(ch);

                    buffers.outputBuffer->write(dummyPointers.data(), numChannels, 480, 48000.0);
                }

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

    // getAvailableDeviceTypes() calls scanDevicesIfNeeded() which performs the first scan once.
    // Do not call type->scanForDevices() here — that would trigger a full OS hardware rescan
    // (WASAPI/ASIO/DirectSound) on every call, causing ~100 ms stalls on the OBS main thread.
    std::map<juce::String, juce::StringArray> devicesByType;
    for (auto& type : enumerator->getAvailableDeviceTypes())
    {
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

    // getAvailableDeviceTypes() calls scanDevicesIfNeeded() which performs the first scan once.
    // Do not call type->scanForDevices() here — see getInputDevicesByType() for details.
    std::map<juce::String, juce::StringArray> devicesByType;
    for (auto& type : enumerator->getAvailableDeviceTypes())
    {
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
            auto* device = it->second->getCurrentDevice();
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

    // Lazily initialize device enumerator with thread safety (only for devices that aren't already
    // open)
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
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        juce::String deviceKey = findDeviceKeyByName(deviceName);
        auto it = deviceHandlers.find(deviceKey);
        if (it != deviceHandlers.end() && it->second->isDeviceOpen())
        {
            if (auto* device = it->second->getCurrentDevice())
                return device->getAvailableSampleRates();
        }
    }

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
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        juce::String deviceKey = findDeviceKeyByName(deviceName);
        auto it = deviceHandlers.find(deviceKey);
        if (it != deviceHandlers.end() && it->second->isDeviceOpen())
        {
            if (auto* device = it->second->getCurrentDevice())
                return device->getAvailableBufferSizes();
        }
    }

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

int AudioServer::getDefaultBufferSize(const juce::String& deviceName) const
{
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        juce::String deviceKey = findDeviceKeyByName(deviceName);
        auto it = deviceHandlers.find(deviceKey);
        if (it != deviceHandlers.end() && it->second->isDeviceOpen())
            return it->second->getBufferSize();
    }

    double sampleRate = 0.0;
    int bufferSize = 0;
    AudioDeviceEnumerator::getCurrentHardwareSetup(deviceName, sampleRate, bufferSize);
    return bufferSize;
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
    auto handler = std::make_unique<AudioDeviceHandler>(actualDeviceName, deviceKey);
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

    std::lock_guard<std::mutex> lock(devicesMutex);

    // Find or create device key from device name
    juce::String deviceKey = findDeviceKeyByName(deviceName);

    // Get or create device handler
    auto* handler = getOrCreateDeviceHandler(deviceKey);
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

    // Open device if not already open
    // Check if device needs to be reopened due to parameter changes
    bool needsReopen = false;
    if (handler->isDeviceOpen())
    {
        auto* device = handler->getCurrentDevice();
        if (device == nullptr)
        {
            // Handler thinks it's open but device is gone (e.g., after hotplug)
            atk::logging::debug(
                "AudioServer::registerDirectCallback",
                "forcing reopen: device missing for \"" + deviceName + "\""
            );
            needsReopen = true;
        }
        else if (!device->isOpen())
        {
            // Device exists but isn't actually open
            atk::logging::debug(
                "AudioServer::registerDirectCallback",
                "forcing reopen: device not open for \"" + deviceName + "\""
            );
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
            atk::logging::warning("AudioServer::registerDirectCallback", "failed to open device \"" + deviceName + "\"");
            return false;
        }
    }
    else if (auto* device = handler->getCurrentDevice())
    {
        if (!device->isPlaying())
        {
            atk::logging::warning(
                "AudioServer::registerDirectCallback",
                "device is open but not playing for \"" + deviceName + "\""
            );
        }
    }

    atk::logging::info("AudioServer::registerDirectCallback", "registered direct callback for \"" + deviceName + "\"");
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

        if (!it->second->hasDesiredSubscriptions())
            it->second->closeDevice();
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
        atk::logging::warning("AudioServer::setDeviceSampleRate", "handler not found for \"" + deviceName + "\"");
        return false;
    }

    auto& handler = it->second;
    if (!handler->isDeviceOpen())
    {
        // State restore can run before the device is lazily opened, so defer instead of dropping.
        auto pendingSetup = handler->getPreferredRecoverySetup();
        pendingSetup.sampleRate = newSampleRate;
        handler->updatePreferredRecoverySetup(pendingSetup);

        atk::logging::debug(
            "AudioServer::setDeviceSampleRate",
            juce::String::formatted("device not open, deferring sample rate %.2f for \"", newSampleRate)
                + deviceName
                + "\""
        );
        return true;
    }

    auto* device = handler->getCurrentDevice();
    if (!device)
    {
        atk::logging::warning("AudioServer::setDeviceSampleRate", "current device missing for \"" + deviceName + "\"");
        return false;
    }

    // Check if the rate is supported
    auto availableRates = device->getAvailableSampleRates();
    if (!availableRates.contains(newSampleRate))
    {
        atk::logging::warning(
            "AudioServer::setDeviceSampleRate",
            juce::String::formatted("unsupported sample rate %.2f for \"", newSampleRate) + deviceName + "\""
        );
        return false;
    }

    atk::logging::debug(
        "AudioServer::setDeviceSampleRate",
        juce::String::formatted("applying sample rate %.2f to \"", newSampleRate) + deviceName + "\""
    );
    juce::AudioDeviceManager::AudioDeviceSetup newSetup;
    newSetup.outputDeviceName =
        device->getActiveOutputChannels().countNumberOfSetBits() > 0 ? deviceName : juce::String();
    newSetup.inputDeviceName =
        device->getActiveInputChannels().countNumberOfSetBits() > 0 ? deviceName : juce::String();
    newSetup.bufferSize = device->getCurrentBufferSizeSamples();
    newSetup.inputChannels = device->getActiveInputChannels();
    newSetup.outputChannels = device->getActiveOutputChannels();
    newSetup.sampleRate = newSampleRate;
    handler->updatePreferredRecoverySetup(newSetup);

    handler->closeDevice();
    if (!handler->openDevice(newSetup))
    {
        atk::logging::warning("AudioServer::setDeviceSampleRate", "failed to reopen device after sample rate change");
        return false;
    }

    atk::logging::info(
        "AudioServer::setDeviceSampleRate",
        juce::String::formatted("sample rate updated to %.2f for \"", newSampleRate) + deviceName + "\""
    );
    return true;
}

bool AudioServer::setDeviceBufferSize(const juce::String& deviceName, int newBufferSize)
{
    std::lock_guard<std::mutex> lock(devicesMutex);

    juce::String deviceKey = findDeviceKeyByName(deviceName);
    auto it = deviceHandlers.find(deviceKey);
    if (it == deviceHandlers.end())
    {
        atk::logging::warning("AudioServer::setDeviceBufferSize", "handler not found for \"" + deviceName + "\"");
        return false;
    }

    auto& handler = it->second;
    if (!handler->isDeviceOpen())
    {
        // State restore can run before the device is lazily opened, so defer instead of dropping.
        auto pendingSetup = handler->getPreferredRecoverySetup();
        pendingSetup.bufferSize = newBufferSize;
        handler->updatePreferredRecoverySetup(pendingSetup);

        atk::logging::debug(
            "AudioServer::setDeviceBufferSize",
            juce::String::formatted("device not open, deferring buffer size %d for \"", newBufferSize)
                + deviceName
                + "\""
        );
        return true;
    }

    auto* device = handler->getCurrentDevice();
    if (!device)
    {
        atk::logging::warning("AudioServer::setDeviceBufferSize", "current device missing for \"" + deviceName + "\"");
        return false;
    }

    // Check if the buffer size is supported
    auto availableSizes = device->getAvailableBufferSizes();
    if (!availableSizes.contains(newBufferSize))
    {
        atk::logging::warning(
            "AudioServer::setDeviceBufferSize",
            juce::String::formatted("unsupported buffer size %d for \"", newBufferSize) + deviceName + "\""
        );
        return false;
    }

    atk::logging::debug(
        "AudioServer::setDeviceBufferSize",
        juce::String::formatted("applying buffer size %d to \"", newBufferSize) + deviceName + "\""
    );
    juce::AudioDeviceManager::AudioDeviceSetup newSetup;
    newSetup.outputDeviceName =
        device->getActiveOutputChannels().countNumberOfSetBits() > 0 ? deviceName : juce::String();
    newSetup.inputDeviceName =
        device->getActiveInputChannels().countNumberOfSetBits() > 0 ? deviceName : juce::String();
    newSetup.sampleRate = device->getCurrentSampleRate();
    newSetup.inputChannels = device->getActiveInputChannels();
    newSetup.outputChannels = device->getActiveOutputChannels();
    newSetup.bufferSize = newBufferSize;
    handler->updatePreferredRecoverySetup(newSetup);

    handler->closeDevice();
    if (!handler->openDevice(newSetup))
    {
        atk::logging::warning("AudioServer::setDeviceBufferSize", "failed to reopen device after buffer size change");
        return false;
    }

    atk::logging::info(
        "AudioServer::setDeviceBufferSize",
        juce::String::formatted("buffer size updated to %d for \"", newBufferSize) + deviceName + "\""
    );
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

    auto* device = handler->getCurrentDevice();
    if (!device)
        return false;

    outSetup.outputDeviceName =
        device->getActiveOutputChannels().countNumberOfSetBits() > 0 ? deviceName : juce::String();
    outSetup.inputDeviceName =
        device->getActiveInputChannels().countNumberOfSetBits() > 0 ? deviceName : juce::String();
    outSetup.sampleRate = device->getCurrentSampleRate();
    outSetup.bufferSize = device->getCurrentBufferSizeSamples();
    outSetup.inputChannels = device->getActiveInputChannels();
    outSetup.outputChannels = device->getActiveOutputChannels();

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
