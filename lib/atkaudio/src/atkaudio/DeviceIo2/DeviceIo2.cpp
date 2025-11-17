#include "DeviceIo2.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServerSettingsComponent.h>
#include <atkaudio/ModuleInfrastructure/AudioServer/ChannelRoutingMatrix.h>
#include <atkaudio/LookAndFeel.h>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

struct atk::DeviceIo2::Impl : public juce::AsyncUpdater
{
    // AudioClient for device I/O routing
    atk::AudioClient audioClient;

    // Channel routing matrix
    atk::ChannelRoutingMatrix routingMatrix;

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

    // Mix Input and Output Delay
    std::atomic_bool mixInput{false};
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
        // ChannelRoutingMatrix initializes with default stereo pass-through
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

        // Update mapping matrices to match new channel count
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

    void setMixInput(bool val)
    {
        mixInput.store(val, std::memory_order_release);
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

        // Copy OBS input to internal buffer
        internalBuffer.clear();
        for (int ch = 0; ch < numChannels; ++ch)
            internalBuffer.copyFrom(ch, 0, buffer[ch], numSamples);

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
        // Note: This will mix device inputs into internalBuffer, which already contains OBS input
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
            buffer,             // OBS buffer (will be modified based on mixInput)
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

        // Handle Mix Input mode for OBS output
        // mixInput determines whether to mix or replace OBS input with processed audio
        bool shouldMix = mixInput.load(std::memory_order_acquire);
        if (shouldMix)
        {
            // Mix mode: OBS output already contains result from applyOutputRouting
            // which includes both OBS input and device inputs routed through internal buffer
            // No additional action needed - buffer already has the mixed result
        }
        else
        {
            // Replace mode: Copy only the processed internal buffer to OBS output
            // This effectively replaces OBS input with the device/processed audio
            for (int ch = 0; ch < numChannels; ++ch)
                std::memcpy(buffer[ch], internalBuffer.getReadPointer(ch), numSamples * sizeof(float));
        }
    }

    juce::Component* getWindowComponent()
    {
        // Lazy creation of settings window
        if (settingsWindow == nullptr)
        {
            class SettingsWindow : public juce::DocumentWindow
            {
            public:
                SettingsWindow(atk::AudioClient* client)
                    : juce::DocumentWindow(
                          "DeviceIo2 Audio Settings",
                          juce::LookAndFeel::getDefaultLookAndFeel().findColour(
                              juce::ResizableWindow::backgroundColourId
                          ),
                          juce::DocumentWindow::closeButton
                      )
                {
                    setUsingNativeTitleBar(true);
                    setResizable(true, false);

                    auto* audioComponent = new atk::AudioServerSettingsComponent(client);

                    // Set up channel names based on current OBS channel count
                    juce::StringArray channelNames;
                    for (int i = 0; i < numChannels; ++i)
                        channelNames.add(juce::String(i + 1));

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
                int numChannels;
            };

            settingsWindow = std::make_unique<SettingsWindow>(&audioClient);
            // Initialize with current channel count
            if (auto* audioComponent =
                    dynamic_cast<atk::AudioServerSettingsComponent*>(settingsWindow->getContentComponent()))
            {
                juce::StringArray channelNames;
                for (int i = 0; i < currentNumChannels; ++i)
                    channelNames.add(juce::String(i + 1));

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

                // Restore current mappings to UI
                audioComponent->setObsChannelMappings(getInputChannelMapping(), getOutputChannelMapping());
            }
        }

        return settingsWindow.get();
    }

    juce::Component* createEmbeddableSettingsComponent()
    {
        // Create a new AudioServerSettingsComponent that can be embedded
        auto* audioComponent = new atk::AudioServerSettingsComponent(&audioClient);

        // Set up channel names based on current OBS channel count
        juce::StringArray channelNames;
        for (int i = 0; i < currentNumChannels; ++i)
            channelNames.add(juce::String(i + 1));

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

        // Restore current mappings to UI
        audioComponent->setObsChannelMappings(getInputChannelMapping(), getOutputChannelMapping());

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

        // Restore OBS channel mappings
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

        // Restore AudioClient subscriptions
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
    }
};

atk::DeviceIo2::DeviceIo2()
{
    pImpl = new Impl();
}

atk::DeviceIo2::~DeviceIo2()
{
    delete pImpl;
}

void atk::DeviceIo2::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::DeviceIo2::setMixInput(bool mixInput)
{
    pImpl->setMixInput(mixInput);
}

void atk::DeviceIo2::setOutputDelay(float delayMs)
{
    pImpl->setOutputDelay(delayMs);
}

float atk::DeviceIo2::getOutputDelay() const
{
    return pImpl->getOutputDelay();
}

void atk::DeviceIo2::setInputChannelMapping(const std::vector<std::vector<bool>>& mapping)
{
    pImpl->setInputChannelMapping(mapping);
}

std::vector<std::vector<bool>> atk::DeviceIo2::getInputChannelMapping() const
{
    return pImpl->getInputChannelMapping();
}

void atk::DeviceIo2::setOutputChannelMapping(const std::vector<std::vector<bool>>& mapping)
{
    pImpl->setOutputChannelMapping(mapping);
}

std::vector<std::vector<bool>> atk::DeviceIo2::getOutputChannelMapping() const
{
    return pImpl->getOutputChannelMapping();
}

void atk::DeviceIo2::getState(std::string& s)
{
    pImpl->getState(s);
}

void atk::DeviceIo2::setState(std::string& s)
{
    pImpl->setState(s);
}

juce::Component* atk::DeviceIo2::getWindowComponent()
{
    return pImpl->getWindowComponent();
}

juce::Component* atk::DeviceIo2::createEmbeddableSettingsComponent()
{
    return pImpl->createEmbeddableSettingsComponent();
}
