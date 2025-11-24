#include "DeviceIo2.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServerSettingsComponent.h>
#include <atkaudio/ModuleInfrastructure/AudioServer/ChannelRoutingMatrix.h>
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleBridge.h>
#include <atkaudio/LookAndFeel.h>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

struct atk::DeviceIo2::Impl : public juce::AsyncUpdater
{
    // AudioClient for device I/O routing
    atk::AudioClient audioClient;

    // Channel routing matrix
    atk::ChannelRoutingMatrix routingMatrix;

    // Device manager for audio device configuration (like PluginHost2)
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<atk::ModuleDeviceManager> moduleDeviceManager;

    // Buffers for device I/O
    juce::AudioBuffer<float> deviceInputBuffer;
    juce::AudioBuffer<float> deviceOutputBuffer;
    juce::AudioBuffer<float> internalBuffer;

    // Settings window
    std::unique_ptr<juce::DocumentWindow> settingsWindow;

    // Current OBS channel count
    int currentNumChannels = 2; // Default to stereo

    // Prepared state for realtime-safe processing
    int preparedNumChannels = 0;
    int preparedNumSamples = 0;
    double preparedSampleRate = 0.0;

    // Output Delay
    std::atomic<float> outputDelayMs{0.0f};
    std::vector<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>> outputDelayLines;
    std::vector<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>> outputDelaySmooth;
    bool delayPrepared = false;

    // Pending async updates (access only from message thread)
    enum class UpdateType
    {
        None,
        ChannelInfo,
        InputBufferResize,
        OutputBufferResize
    };
    std::atomic<UpdateType> pendingUpdateType{UpdateType::None};
    std::atomic<int> pendingNumChannels{0};
    std::atomic<int> pendingNumSamples{0};
    std::atomic<int> pendingInputSubs{0};
    std::atomic<int> pendingOutputSubs{0};

    Impl()
    {
        // AudioClient automatically registers with AudioServer

        // Create ModuleDeviceManager for audio device configuration (like PluginHost2)
        moduleDeviceManager = std::make_unique<atk::ModuleDeviceManager>(
            std::make_unique<atk::ModuleAudioIODeviceType>("DeviceIo2 Audio"),
            deviceManager
        );

        // Initialize device management - don't open OBS device, just make it available
        moduleDeviceManager->initialize();

        // Initialize default diagonal OBS routing (will be resized when process() is called)
        // Start with stereo as default, will auto-resize to match incoming OBS channel count
        routingMatrix.initializeDefaultMapping(2);
    }

    ~Impl()
    {
        // Cancel any pending async updates
        cancelPendingUpdate();

        // Clean up settings window asynchronously on the message thread
        auto* settingsWin = this->settingsWindow.release();
        auto lambda = [settingsWin] { delete settingsWin; };
        juce::MessageManager::callAsync(lambda);

        // AtomicSharedPtr cleans up automatically
    }

    void handleAsyncUpdate() override
    {
        // Called on message thread to handle async updates safely
        auto updateType = pendingUpdateType.exchange(UpdateType::None);

        switch (updateType)
        {
        case UpdateType::ChannelInfo:
        {
            int numChannels = pendingNumChannels.load();
            updateChannelInfoOnMessageThread(numChannels);
            break;
        }
        case UpdateType::InputBufferResize:
        {
            int numSubs = pendingInputSubs.load();
            int numSamples = pendingNumSamples.load();
            if (deviceInputBuffer.getNumChannels() < numSubs || deviceInputBuffer.getNumSamples() < numSamples)
                deviceInputBuffer.setSize(std::max(numSubs, 1), numSamples, false, false, true);
            break;
        }
        case UpdateType::OutputBufferResize:
        {
            int numSubs = pendingOutputSubs.load();
            int numSamples = pendingNumSamples.load();
            if (deviceOutputBuffer.getNumChannels() < numSubs || deviceOutputBuffer.getNumSamples() < numSamples)
                deviceOutputBuffer.setSize(std::max(numSubs, 1), numSamples, false, false, true);
            break;
        }
        case UpdateType::None:
        default:
            break;
        }
    }

    void updateChannelInfoOnMessageThread(int numChannels)
    {
        // Update stored channel count
        currentNumChannels = numChannels;

        // Allocate internal buffer for the current configuration
        if (internalBuffer.getNumChannels() < numChannels || internalBuffer.getNumSamples() < preparedNumSamples)
            internalBuffer.setSize(numChannels, preparedNumSamples, false, false, true);

        // Check if routing matrix needs resizing
        // The matrix size should be: numOBSChannels + numDeviceSubscriptions
        auto currentSubs = audioClient.getSubscriptions();
        int numInputSubs = (int)currentSubs.inputSubscriptions.size();
        int numOutputSubs = (int)currentSubs.outputSubscriptions.size();
        int expectedInputRows = numChannels + numInputSubs;
        int expectedOutputRows = numChannels + numOutputSubs;

        auto currentInput = getInputChannelMapping();
        auto currentOutput = getOutputChannelMapping();

        // Only resize if the matrix doesn't match expected size
        // resizeMappings is designed for OBS channel changes, not for adding/removing device subscriptions
        bool needsResize = ((int)currentInput.size() != expectedInputRows && expectedInputRows == numChannels)
                        || ((int)currentOutput.size() != expectedOutputRows && expectedOutputRows == numChannels);

        if (needsResize)
            routingMatrix.resizeMappings(numChannels);

        // If settings window is already created, update it
        if (settingsWindow != nullptr)
        {
            // Find the AudioServerSettingsComponent inside the window
            if (auto* audioComponent =
                    dynamic_cast<atk::AudioServerSettingsComponent*>(settingsWindow->getContentComponent()))
            {
                juce::StringArray channelNames;
                for (int i = 0; i < numChannels; ++i)
                    channelNames.add(juce::String(i + 1));

                audioComponent->setClientChannelInfo(channelNames, channelNames, "DeviceIo2");

                // Update OBS mappings in UI
                audioComponent->setObsChannelMappings(getInputChannelMapping(), getOutputChannelMapping());
            }
        }
    }

    void setOutputDelay(float delayMs)
    {
        outputDelayMs.store(delayMs, std::memory_order_release);
    }

    float getOutputDelay() const
    {
        return outputDelayMs.load(std::memory_order_acquire);
    }

    void prepareOutputDelay(int numChannels, int numSamples, double sampleRate)
    {
        outputDelayLines.clear();
        outputDelaySmooth.clear();

        // Max delay: 10000ms = 10 seconds
        const float maxDelaySeconds = 10.0f;
        const int maxDelaySamples = static_cast<int>(maxDelaySeconds * sampleRate);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine(maxDelaySamples);
            delayLine.reset();
            delayLine.prepare({sampleRate, (juce::uint32)numSamples, (juce::uint32)1});
            outputDelayLines.push_back(std::move(delayLine));

            juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smooth;
            smooth.reset(sampleRate, 0.05); // 50ms smoothing time
            outputDelaySmooth.push_back(smooth);
        }

        delayPrepared = true;
    }

    void applyOutputDelay(juce::AudioBuffer<float>& buffer, int numChannels, int numSamples, double sampleRate)
    {
        // Prepare delay lines if not ready or parameters changed
        if (!delayPrepared || outputDelayLines.size() != numChannels)
            prepareOutputDelay(numChannels, numSamples, sampleRate);

        // Get current delay setting
        float delayMs = outputDelayMs.load(std::memory_order_acquire);
        float delaySamples = (delayMs / 1000.0f) * static_cast<float>(sampleRate);

        // Apply delay to each channel
        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (ch < outputDelayLines.size())
            {
                // Set target delay value
                outputDelaySmooth[ch].setTargetValue(delaySamples);

                auto* channelData = buffer.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    outputDelayLines[ch].pushSample(0, channelData[i]);
                    float currentDelay = outputDelaySmooth[ch].getNextValue();
                    channelData[i] = outputDelayLines[ch].popSample(0, currentDelay);
                }
            }
        }
    }

    void process(float** buffer, int numChannels, int numSamples, double sampleRate)
    {
        // Check if we need reconfiguration (realtime-safe checks only)
        // Note: numSamples can vary between calls, only reallocate if we need MORE space
        bool needsReconfiguration =
            preparedNumChannels != numChannels || preparedNumSamples < numSamples || preparedSampleRate != sampleRate;

        if (needsReconfiguration)
        {
            preparedNumChannels = numChannels;
            preparedNumSamples = juce::jmax(preparedNumSamples, numSamples); // Allocate for the largest size seen
            preparedSampleRate = sampleRate;

            // Schedule async channel info update (can't block OBS audio thread)
            pendingNumChannels.store(numChannels);
            pendingUpdateType.store(UpdateType::ChannelInfo);
            triggerAsyncUpdate();

            // Skip this buffer while preparing
            return;
        }

        // Ensure internal buffer is large enough (realtime-safe if already allocated)
        if (internalBuffer.getNumChannels() < numChannels || internalBuffer.getNumSamples() < numSamples)
        {
            // Buffer not yet allocated, skip this frame
            return;
        }

        // Clear internal buffer - routing matrix will populate it based on mappings
        internalBuffer.clear();

        // Get current subscriptions from AudioClient
        auto clientState = audioClient.getSubscriptions();
        int numInputSubs = (int)clientState.inputSubscriptions.size();
        int numOutputSubs = (int)clientState.outputSubscriptions.size();

        // Ensure device buffers have enough channels (realtime-safe if already allocated)
        if (deviceInputBuffer.getNumChannels() < numInputSubs || deviceInputBuffer.getNumSamples() < numSamples)
        {
            // Need to allocate, schedule async and skip this frame
            pendingInputSubs.store(numInputSubs);
            pendingNumSamples.store(numSamples);
            pendingUpdateType.store(UpdateType::InputBufferResize);
            triggerAsyncUpdate();
            return;
        }

        if (deviceOutputBuffer.getNumChannels() < numOutputSubs || deviceOutputBuffer.getNumSamples() < numSamples)
        {
            // Need to allocate, schedule async and skip this frame
            pendingOutputSubs.store(numOutputSubs);
            pendingNumSamples.store(numSamples);
            pendingUpdateType.store(UpdateType::OutputBufferResize);
            triggerAsyncUpdate();
            return;
        }

        // Pull subscribed device inputs (AudioServer handles the routing based on subscriptions)
        audioClient.pullSubscribedInputs(deviceInputBuffer, numSamples, sampleRate);

        // Apply INPUT routing matrix using ChannelRoutingMatrix
        // Routes OBS inputs and device inputs to internal buffer based on matrix configuration
        routingMatrix.applyInputRouting(
            buffer,            // OBS buffer
            deviceInputBuffer, // Device inputs
            internalBuffer,    // Target buffer
            numChannels,       // Number of OBS channels
            numSamples,        // Number of samples
            numInputSubs       // Number of device input subscriptions
        );

        // Apply OUTPUT routing matrix using ChannelRoutingMatrix
        routingMatrix.applyOutputRouting(
            internalBuffer,     // Source buffer
            buffer,             // OBS buffer
            deviceOutputBuffer, // Device outputs
            numChannels,        // Number of OBS channels
            numSamples,         // Number of samples
            numOutputSubs       // Number of device output subscriptions
        );

        // Apply output delay before sending to hardware
        if (numOutputSubs > 0)
            applyOutputDelay(deviceOutputBuffer, numOutputSubs, numSamples, sampleRate);

        // Push to subscribed device outputs
        audioClient.pushSubscribedOutputs(deviceOutputBuffer, numSamples, sampleRate);

        // Output routing matrix has already populated the OBS buffer based on routing configuration
        // No additional processing needed - the matrix controls what goes to OBS output
    }

    juce::Component* getWindowComponent()
    {
        // Lazy creation of settings window
        if (settingsWindow == nullptr)
        {
            class SettingsWindow : public juce::DocumentWindow
            {
            public:
                SettingsWindow(atk::AudioClient* client, juce::AudioDeviceManager* devManager, int numCh)
                    : juce::DocumentWindow(
                          "DeviceIo2 Audio Settings",
                          juce::LookAndFeel::getDefaultLookAndFeel().findColour(
                              juce::ResizableWindow::backgroundColourId
                          ),
                          juce::DocumentWindow::closeButton
                      )
                    , numChannels(numCh)
                {
                    setUsingNativeTitleBar(true);
                    setResizable(true, false);

                    auto* audioComponent = new atk::AudioServerSettingsComponent(client);

                    // Set DeviceIo2's deviceManager for Device... button
                    audioComponent->setDeviceManager(devManager);

                    // Set up channel names based on current OBS channel count
                    juce::StringArray channelNames;
                    for (int i = 0; i < numChannels; ++i)
                        channelNames.add(juce::String(i + 1));

                    // Set up fixed top rows for OBS channels BEFORE setClientChannelInfo
                    audioComponent->setInputFixedTopRows(channelNames, true); // true = apply default diagonal
                    audioComponent->setOutputFixedTopRows(channelNames, true);

                    audioComponent->setClientChannelInfo(channelNames, channelNames, "DeviceIo2");

                    setContentOwned(audioComponent, true);

                    centreWithSize(900, 700);
                }

                void closeButtonPressed() override
                {
                    setVisible(false);
                }

            private:
                juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;
                int numChannels = 0;
            };

            settingsWindow = std::make_unique<SettingsWindow>(&audioClient, &deviceManager, currentNumChannels);
            // Initialize with current channel count
            if (auto* audioComponent =
                    dynamic_cast<atk::AudioServerSettingsComponent*>(settingsWindow->getContentComponent()))
            {
                juce::StringArray channelNames;
                for (int i = 0; i < currentNumChannels; ++i)
                    channelNames.add(juce::String(i + 1));

                // Set up fixed top rows for OBS channels BEFORE setting mappings
                // This configures the matrix to have OBS channel rows that can be mapped
                audioComponent->setInputFixedTopRows(channelNames, true); // true = apply default diagonal
                audioComponent->setOutputFixedTopRows(channelNames, true);

                audioComponent->setClientChannelInfo(channelNames, channelNames, "DeviceIo2");

                // Set callback to apply OBS channel mapping when user clicks Apply
                audioComponent->onObsMappingChanged = [this](
                                                          const std::vector<std::vector<bool>>& inputMapping,
                                                          const std::vector<std::vector<bool>>& outputMapping
                                                      )
                {
                    setInputChannelMapping(inputMapping);
                    setOutputChannelMapping(outputMapping);
                };

                // Set callback to get current OBS mappings for Restore button
                audioComponent->getCurrentObsMappings =
                    [this]() -> std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>>
                { return {getInputChannelMapping(), getOutputChannelMapping()}; };

                // Restore complete routing matrix (OBS + device subscription rows)
                // This must happen AFTER setInputFixedTopRows/setOutputFixedTopRows
                // Use setCompleteRoutingMatrices instead of setObsChannelMappings to restore ALL rows
                audioComponent->setCompleteRoutingMatrices(getInputChannelMapping(), getOutputChannelMapping());
            }
        }

        return settingsWindow.get();
    }

    juce::Component* createEmbeddableSettingsComponent()
    {
        // Create a new AudioServerSettingsComponent that can be embedded
        auto* audioComponent = new atk::AudioServerSettingsComponent(&audioClient);

        // Set DeviceIo2's deviceManager for Device... button (shows AudioServer devices like PluginHost2)
        audioComponent->setDeviceManager(&deviceManager);

        // Set up channel names based on current OBS channel count
        juce::StringArray channelNames;
        for (int i = 0; i < currentNumChannels; ++i)
            channelNames.add(juce::String(i + 1));

        // Set up fixed top rows for OBS channels BEFORE setting mappings
        // This configures the matrix to have OBS channel rows that can be mapped
        audioComponent->setInputFixedTopRows(channelNames, true); // true = apply default diagonal
        audioComponent->setOutputFixedTopRows(channelNames, true);

        audioComponent->setClientChannelInfo(channelNames, channelNames, "DeviceIo2");

        // Set callback to apply OBS channel mapping when user clicks Apply
        audioComponent->onObsMappingChanged = [this](
                                                  const std::vector<std::vector<bool>>& inputMapping,
                                                  const std::vector<std::vector<bool>>& outputMapping
                                              )
        {
            setInputChannelMapping(inputMapping);
            setOutputChannelMapping(outputMapping);
        };

        // Set callback to get current OBS mappings for Restore button
        audioComponent->getCurrentObsMappings =
            [this]() -> std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>>
        { return {getInputChannelMapping(), getOutputChannelMapping()}; };

        // Restore complete routing matrix (OBS + device subscription rows)
        // Subscription rows are now initialized immediately in AudioServerSettingsComponent constructor
        audioComponent->setCompleteRoutingMatrices(getInputChannelMapping(), getOutputChannelMapping());

        return audioComponent;
    }

    void setInputChannelMapping(const std::vector<std::vector<bool>>& mapping)
    {
        routingMatrix.setInputMapping(mapping);
    }

    std::vector<std::vector<bool>> getInputChannelMapping() const
    {
        return routingMatrix.getInputMapping();
    }

    void setOutputChannelMapping(const std::vector<std::vector<bool>>& mapping)
    {
        routingMatrix.setOutputMapping(mapping);
    }

    std::vector<std::vector<bool>> getOutputChannelMapping() const
    {
        return routingMatrix.getOutputMapping();
    }

    void getState(std::string& s)
    {
        auto rootElement = std::make_unique<juce::XmlElement>("DeviceIo2State");

        // Save output delay
        rootElement->setAttribute("outputDelayMs", outputDelayMs.load(std::memory_order_acquire));

        // Save OBS channel mappings
        auto inputMapping = routingMatrix.getInputMapping();
        auto outputMapping = routingMatrix.getOutputMapping();

        if (!inputMapping.empty())
        {
            auto* inputMappingElement = new juce::XmlElement("InputMapping");
            for (size_t i = 0; i < inputMapping.size(); ++i)
            {
                auto* rowElement = new juce::XmlElement("Row");
                juce::String rowData;
                for (size_t j = 0; j < inputMapping[i].size(); ++j)
                    rowData += inputMapping[i][j] ? "1" : "0";
                rowElement->setAttribute("data", rowData);
                inputMappingElement->addChildElement(rowElement);
            }
            rootElement->addChildElement(inputMappingElement);
        }

        if (!outputMapping.empty())
        {
            auto* outputMappingElement = new juce::XmlElement("OutputMapping");
            for (size_t i = 0; i < outputMapping.size(); ++i)
            {
                auto* rowElement = new juce::XmlElement("Row");
                juce::String rowData;
                for (size_t j = 0; j < outputMapping[i].size(); ++j)
                    rowData += outputMapping[i][j] ? "1" : "0";
                rowElement->setAttribute("data", rowData);
                outputMappingElement->addChildElement(rowElement);
            }
            rootElement->addChildElement(outputMappingElement);
        }

        // Save AudioClient subscriptions
        auto* subscriptionsElement = new juce::XmlElement("Subscriptions");
        auto clientState = audioClient.getSubscriptions();

        // Save input subscriptions
        for (const auto& sub : clientState.inputSubscriptions)
        {
            auto* subElement = new juce::XmlElement("InputSub");
            subElement->setAttribute("device", sub.deviceName);
            subElement->setAttribute("deviceType", sub.deviceType);
            subElement->setAttribute("channel", sub.channelIndex);
            subscriptionsElement->addChildElement(subElement);
        }

        // Save output subscriptions
        for (const auto& sub : clientState.outputSubscriptions)
        {
            auto* subElement = new juce::XmlElement("OutputSub");
            subElement->setAttribute("device", sub.deviceName);
            subElement->setAttribute("deviceType", sub.deviceType);
            subElement->setAttribute("channel", sub.channelIndex);
            subscriptionsElement->addChildElement(subElement);
        }

        rootElement->addChildElement(subscriptionsElement);

        auto stateString = rootElement->toString().toStdString();
        s = stateString;
    }

    void setState(std::string& s)
    {
        if (s.empty())
            return;

        juce::XmlDocument chunkDataXml(s);
        auto rootElement = chunkDataXml.getDocumentElement();
        if (!rootElement)
            return;

        // Restore output delay
        if (rootElement->hasAttribute("outputDelayMs"))
        {
            float delayMs = static_cast<float>(rootElement->getDoubleAttribute("outputDelayMs"));
            outputDelayMs.store(delayMs, std::memory_order_release);
        }

        // Restore AudioClient subscriptions FIRST
        // This must happen before restoring routing matrix because matrix size depends on subscription count
        if (auto* subscriptionsElement = rootElement->getChildByName("Subscriptions"))
        {
            atk::AudioClientState state;

            for (auto* subElement : subscriptionsElement->getChildIterator())
            {
                atk::ChannelSubscription sub;
                sub.deviceName = subElement->getStringAttribute("device");
                sub.deviceType = subElement->getStringAttribute("deviceType");
                sub.channelIndex = subElement->getIntAttribute("channel");

                if (subElement->hasTagName("InputSub"))
                {
                    sub.isInput = true;
                    state.inputSubscriptions.push_back(sub);
                }
                else if (subElement->hasTagName("OutputSub"))
                {
                    sub.isInput = false;
                    state.outputSubscriptions.push_back(sub);
                }
            }

            audioClient.setSubscriptions(state);
        }

        // Restore complete routing matrix (OBS channels + device subscription channels)
        // The matrix must include rows for both OBS and subscribed device channels
        if (auto* inputMappingElement = rootElement->getChildByName("InputMapping"))
        {
            std::vector<std::vector<bool>> inputMapping;
            for (auto* rowElement : inputMappingElement->getChildIterator())
            {
                juce::String rowData = rowElement->getStringAttribute("data");
                std::vector<bool> row;
                for (int i = 0; i < rowData.length(); ++i)
                    row.push_back(rowData[i] == '1');
                inputMapping.push_back(row);
            }
            if (!inputMapping.empty())
                setInputChannelMapping(inputMapping);
        }

        if (auto* outputMappingElement = rootElement->getChildByName("OutputMapping"))
        {
            std::vector<std::vector<bool>> outputMapping;
            for (auto* rowElement : outputMappingElement->getChildIterator())
            {
                juce::String rowData = rowElement->getStringAttribute("data");
                std::vector<bool> row;
                for (int i = 0; i < rowData.length(); ++i)
                    row.push_back(rowData[i] == '1');
                outputMapping.push_back(row);
            }
            if (!outputMapping.empty())
                setOutputChannelMapping(outputMapping);
        }
    }
};

atk::DeviceIo2::DeviceIo2()
{
    pImpl = new Impl();
}

atk::DeviceIo2::~DeviceIo2()
{
    // Cancel any pending async updates in the implementation
    if (pImpl)
        pImpl->cancelPendingUpdate();

    // Delete implementation asynchronously on message thread to ensure
    // audio processing has fully stopped before AudioClient unregisters
    auto* impl = pImpl;
    pImpl = nullptr; // Clear pointer immediately to prevent further use

    if (impl)
        juce::MessageManager::callAsync([impl]() { delete impl; });
}

void atk::DeviceIo2::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    if (pImpl)
        pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::DeviceIo2::setOutputDelay(float delayMs)
{
    if (pImpl)
        pImpl->setOutputDelay(delayMs);
}

float atk::DeviceIo2::getOutputDelay() const
{
    return pImpl ? pImpl->getOutputDelay() : 0.0f;
}

void atk::DeviceIo2::setInputChannelMapping(const std::vector<std::vector<bool>>& mapping)
{
    if (pImpl)
        pImpl->setInputChannelMapping(mapping);
}

std::vector<std::vector<bool>> atk::DeviceIo2::getInputChannelMapping() const
{
    return pImpl ? pImpl->getInputChannelMapping() : std::vector<std::vector<bool>>();
}

void atk::DeviceIo2::setOutputChannelMapping(const std::vector<std::vector<bool>>& mapping)
{
    if (pImpl)
        pImpl->setOutputChannelMapping(mapping);
}

std::vector<std::vector<bool>> atk::DeviceIo2::getOutputChannelMapping() const
{
    return pImpl ? pImpl->getOutputChannelMapping() : std::vector<std::vector<bool>>();
}

void atk::DeviceIo2::getState(std::string& s)
{
    if (pImpl)
        pImpl->getState(s);
}

void atk::DeviceIo2::setState(std::string& s)
{
    if (pImpl)
        pImpl->setState(s);
}

juce::Component* atk::DeviceIo2::getWindowComponent()
{
    return pImpl ? pImpl->getWindowComponent() : nullptr;
}

juce::Component* atk::DeviceIo2::createEmbeddableSettingsComponent()
{
    return pImpl ? pImpl->createEmbeddableSettingsComponent() : nullptr;
}
