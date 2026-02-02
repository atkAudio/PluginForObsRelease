#include "DeviceIo2.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServerSettingsComponent.h>
#include <atkaudio/ModuleInfrastructure/AudioServer/ChannelRoutingMatrix.h>
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleBridge.h>
#include <atkaudio/LookAndFeel.h>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

struct atk::DeviceIo2::Impl : public juce::AsyncUpdater
{
    atk::AudioClient audioClient;
    atk::ChannelRoutingMatrix routingMatrix;

    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<atk::ModuleDeviceManager> moduleDeviceManager;

    juce::AudioBuffer<float> deviceInputBuffer;
    juce::AudioBuffer<float> deviceOutputBuffer;
    juce::AudioBuffer<float> internalBuffer;

    std::unique_ptr<juce::DocumentWindow> settingsWindow;

    int currentNumChannels = 2;
    int preparedNumChannels = 0;
    int preparedNumSamples = 0;
    double preparedSampleRate = 0.0;

    std::atomic<float> outputDelayMs{0.0f};
    std::vector<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>> outputDelayLines;
    std::vector<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>> outputDelaySmooth;
    bool delayPrepared = false;
    std::atomic<bool> bypass{false};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> fadeGain{1.0f};
    std::atomic<double> fadeDurationSeconds{0.5};

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
        moduleDeviceManager = std::make_unique<atk::ModuleDeviceManager>(
            std::make_unique<atk::ModuleAudioIODeviceType>("DeviceIo2 Audio"),
            deviceManager
        );
        moduleDeviceManager->initialize();
        routingMatrix.initializeDefaultMapping(2);
    }

    ~Impl()
    {
        cancelPendingUpdate();
        auto* settingsWin = this->settingsWindow.release();
        juce::MessageManager::callAsync([settingsWin] { delete settingsWin; });
    }

    void handleAsyncUpdate() override
    {
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
        currentNumChannels = numChannels;

        if (internalBuffer.getNumChannels() < numChannels || internalBuffer.getNumSamples() < preparedNumSamples)
            internalBuffer.setSize(numChannels, preparedNumSamples, false, false, true);

        auto currentSubs = audioClient.getSubscriptions();
        int numInputSubs = (int)currentSubs.inputSubscriptions.size();
        int numOutputSubs = (int)currentSubs.outputSubscriptions.size();
        int expectedInputRows = numChannels + numInputSubs;
        int expectedOutputRows = numChannels + numOutputSubs;

        auto currentInput = getInputChannelMapping();
        auto currentOutput = getOutputChannelMapping();

        bool needsResize = ((int)currentInput.size() != expectedInputRows && expectedInputRows == numChannels)
                        || ((int)currentOutput.size() != expectedOutputRows && expectedOutputRows == numChannels);

        if (needsResize)
            routingMatrix.resizeMappings(numChannels);

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

        const float maxDelaySeconds = 10.0f;
        const int maxDelaySamples = static_cast<int>(maxDelaySeconds * sampleRate);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine(maxDelaySamples);
            delayLine.reset();
            delayLine.prepare({sampleRate, (juce::uint32)numSamples, (juce::uint32)1});
            outputDelayLines.push_back(std::move(delayLine));

            juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smooth;
            smooth.reset(sampleRate, 0.05);
            outputDelaySmooth.push_back(smooth);
        }

        delayPrepared = true;
    }

    void applyOutputDelay(juce::AudioBuffer<float>& buffer, int numChannels, int numSamples, double sampleRate)
    {
        if (!delayPrepared || outputDelayLines.size() != numChannels)
            prepareOutputDelay(numChannels, numSamples, sampleRate);

        float delayMs = outputDelayMs.load(std::memory_order_acquire);
        float delaySamples = (delayMs / 1000.0f) * static_cast<float>(sampleRate);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (ch < outputDelayLines.size())
            {
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
        bool currentBypass = bypass.load(std::memory_order_acquire);
        float targetGain = currentBypass ? 0.0f : 1.0f;

        // Update smoother if target changed
        if (fadeGain.getTargetValue() != targetGain)
        {
            fadeGain.reset(sampleRate, fadeDurationSeconds.load(std::memory_order_acquire));

            // Clear buffers when transitioning from bypass to active
            if (!currentBypass)
                audioClient.clearBuffers();

            fadeGain.setTargetValue(targetGain);
        }

        // Skip processing if fully bypassed
        if (currentBypass && !fadeGain.isSmoothing())
            return;

        // Apply fade gain
        if (fadeGain.isSmoothing())
        {
            for (int i = 0; i < numSamples; ++i)
            {
                float gain = fadeGain.getNextValue();
                for (int ch = 0; ch < numChannels; ++ch)
                    buffer[ch][i] *= gain;
            }
        }
        bool needsReconfiguration =
            preparedNumChannels != numChannels || preparedNumSamples < numSamples || preparedSampleRate != sampleRate;

        if (needsReconfiguration)
        {
            preparedNumChannels = numChannels;
            preparedNumSamples = juce::jmax(preparedNumSamples, numSamples);
            preparedSampleRate = sampleRate;

            pendingNumChannels.store(numChannels);
            pendingUpdateType.store(UpdateType::ChannelInfo);
            triggerAsyncUpdate();
            return;
        }

        if (internalBuffer.getNumChannels() < numChannels || internalBuffer.getNumSamples() < numSamples)
            return;

        internalBuffer.clear();

        auto clientState = audioClient.getSubscriptions();
        int numInputSubs = (int)clientState.inputSubscriptions.size();
        int numOutputSubs = (int)clientState.outputSubscriptions.size();

        if (deviceInputBuffer.getNumChannels() < numInputSubs || deviceInputBuffer.getNumSamples() < numSamples)
        {
            pendingInputSubs.store(numInputSubs);
            pendingNumSamples.store(numSamples);
            pendingUpdateType.store(UpdateType::InputBufferResize);
            triggerAsyncUpdate();
            return;
        }

        if (deviceOutputBuffer.getNumChannels() < numOutputSubs || deviceOutputBuffer.getNumSamples() < numSamples)
        {
            pendingOutputSubs.store(numOutputSubs);
            pendingNumSamples.store(numSamples);
            pendingUpdateType.store(UpdateType::OutputBufferResize);
            triggerAsyncUpdate();
            return;
        }

        audioClient.pullSubscribedInputs(deviceInputBuffer, numSamples, sampleRate);

        routingMatrix
            .applyInputRouting(buffer, deviceInputBuffer, internalBuffer, numChannels, numSamples, numInputSubs);

        routingMatrix
            .applyOutputRouting(internalBuffer, buffer, deviceOutputBuffer, numChannels, numSamples, numOutputSubs);

        if (numOutputSubs > 0)
            applyOutputDelay(deviceOutputBuffer, numOutputSubs, numSamples, sampleRate);

        audioClient.pushSubscribedOutputs(deviceOutputBuffer, numSamples, sampleRate);
    }

    juce::Component* getWindowComponent()
    {
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
                          juce::DocumentWindow::allButtons
                      )
                    , numChannels(numCh)
                {
                    setTitleBarButtonsRequired(juce::DocumentWindow::closeButton, false);
                    setResizable(true, false);

                    auto* audioComponent = new atk::AudioServerSettingsComponent(client);

                    audioComponent->setDeviceManager(devManager);

                    juce::StringArray channelNames;
                    for (int i = 0; i < numChannels; ++i)
                        channelNames.add(juce::String(i + 1));

                    audioComponent->setInputFixedTopRows(channelNames, true);
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
            if (auto* audioComponent =
                    dynamic_cast<atk::AudioServerSettingsComponent*>(settingsWindow->getContentComponent()))
            {
                juce::StringArray channelNames;
                for (int i = 0; i < currentNumChannels; ++i)
                    channelNames.add(juce::String(i + 1));

                audioComponent->setInputFixedTopRows(channelNames, true);
                audioComponent->setOutputFixedTopRows(channelNames, true);
                audioComponent->setClientChannelInfo(channelNames, channelNames, "DeviceIo2");

                audioComponent->onObsMappingChanged = [this](
                                                          const std::vector<std::vector<bool>>& inputMapping,
                                                          const std::vector<std::vector<bool>>& outputMapping
                                                      )
                {
                    setInputChannelMapping(inputMapping);
                    setOutputChannelMapping(outputMapping);
                };

                audioComponent->getCurrentObsMappings =
                    [this]() -> std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>>
                { return {getInputChannelMapping(), getOutputChannelMapping()}; };

                audioComponent->setCompleteRoutingMatrices(getInputChannelMapping(), getOutputChannelMapping());
            }
        }

        return settingsWindow.get();
    }

    juce::Component* createEmbeddableSettingsComponent()
    {
        auto* audioComponent = new atk::AudioServerSettingsComponent(&audioClient);
        audioComponent->setDeviceManager(&deviceManager);

        juce::StringArray channelNames;
        for (int i = 0; i < currentNumChannels; ++i)
            channelNames.add(juce::String(i + 1));

        audioComponent->setInputFixedTopRows(channelNames, true);
        audioComponent->setOutputFixedTopRows(channelNames, true);
        audioComponent->setClientChannelInfo(channelNames, channelNames, "DeviceIo2");

        audioComponent->onObsMappingChanged = [this](
                                                  const std::vector<std::vector<bool>>& inputMapping,
                                                  const std::vector<std::vector<bool>>& outputMapping
                                              )
        {
            setInputChannelMapping(inputMapping);
            setOutputChannelMapping(outputMapping);
        };

        audioComponent->getCurrentObsMappings =
            [this]() -> std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>>
        { return {getInputChannelMapping(), getOutputChannelMapping()}; };

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
        rootElement->setAttribute("outputDelayMs", outputDelayMs.load(std::memory_order_acquire));

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

        auto* subscriptionsElement = new juce::XmlElement("Subscriptions");
        auto clientState = audioClient.getSubscriptions();

        for (const auto& sub : clientState.inputSubscriptions)
        {
            auto* subElement = new juce::XmlElement("InputSub");
            subElement->setAttribute("device", sub.deviceName);
            subElement->setAttribute("deviceType", sub.deviceType);
            subElement->setAttribute("channel", sub.channelIndex);
            subscriptionsElement->addChildElement(subElement);
        }

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

        if (rootElement->hasAttribute("outputDelayMs"))
        {
            float delayMs = static_cast<float>(rootElement->getDoubleAttribute("outputDelayMs"));
            outputDelayMs.store(delayMs, std::memory_order_release);
        }

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
    if (pImpl)
        pImpl->cancelPendingUpdate();

    auto* impl = pImpl;
    pImpl = nullptr;

    if (impl)
        juce::MessageManager::callAsync([impl]() { delete impl; });
}

void atk::DeviceIo2::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    if (pImpl)
        pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::DeviceIo2::setBypass(bool shouldBypass)
{
    if (pImpl)
        pImpl->bypass.store(shouldBypass, std::memory_order_release);
}

bool atk::DeviceIo2::isBypassed() const
{
    return pImpl ? pImpl->bypass.load(std::memory_order_acquire) : false;
}

void atk::DeviceIo2::setFadeTime(double seconds)
{
    if (pImpl)
        pImpl->fadeDurationSeconds.store(seconds, std::memory_order_release);
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
