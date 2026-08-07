#pragma once

#include <atkaudio/DeviceIo2/DeviceIo2.h>
#include <atkaudio/DeviceIo2/DeviceIo2ObsControl.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <string>

class DeviceIo2Plugin final
    : public juce::AudioProcessor
    , private juce::Timer
{
public:
    DeviceIo2Plugin()
        : AudioProcessor(
              BusesProperties()
                  .withInput("Input", juce::AudioChannelSet::stereo())
                  .withOutput("Output", juce::AudioChannelSet::stereo())
          )
    {
        // Lazy init: keep constructor cheap so internal plugin metadata enumeration
        // does not instantiate DeviceIo2 and trigger device probing.
        startTimerHz(30);
    }

    ~DeviceIo2Plugin() override
    {
        stopTimer();
        deviceIo2.reset();
    }

    const juce::String getName() const override
    {
        return "DeviceIo2";
    }

    bool acceptsMidi() const override
    {
        return false;
    }

    bool producesMidi() const override
    {
        return false;
    }

    double getTailLengthSeconds() const override
    {
        return 0.0;
    }

    int getNumPrograms() override
    {
        return 1;
    }

    int getCurrentProgram() override
    {
        return 0;
    }

    void setCurrentProgram(int) override
    {
    }

    const juce::String getProgramName(int) override
    {
        return "Default";
    }

    void changeProgramName(int, const juce::String&) override
    {
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        juce::ignoreUnused(sampleRate, samplesPerBlock);
        ensureDeviceIo2Initialized();
        applyPendingStateIfAny();
        // DeviceIo2 handles its own preparation internally during process()
    }

    void releaseResources() override
    {
        // DeviceIo2 handles its own resource management
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        jassert(deviceIo2 != nullptr);

        if (!deviceIo2)
            return;

        // Convert JUCE buffer to raw pointer format expected by DeviceIo2
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        // Create array of channel pointers
        std::vector<float*> channelPointers(numChannels);
        for (int ch = 0; ch < numChannels; ++ch)
            channelPointers[ch] = buffer.getWritePointer(ch);

        // Process through DeviceIo2
        // INPUT routing: Hardware inputs (selected in INPUT matrix) → mixed into plugin output
        // OUTPUT routing: Plugin input → sent to hardware (selected in OUTPUT matrix)
        deviceIo2->process(channelPointers.data(), numChannels, numSamples, getSampleRate());
    }

    juce::AudioProcessorEditor* createEditor() override
    {
        ensureDeviceIo2Initialized();
        applyPendingStateIfAny();

        // DeviceIo2 provides a method to create an embeddable settings component
        if (deviceIo2)
        {
            if (auto* settingsComponent = deviceIo2->createEmbeddableSettingsComponent())
            {
                // Wrap the settings component in a generic editor
                class DeviceIo2Editor : public juce::AudioProcessorEditor
                {
                public:
                    DeviceIo2Editor(DeviceIo2Plugin& p, juce::Component* content)
                        : AudioProcessorEditor(p)
                        , plugin(p)
                    {
                        // Take ownership of the content component
                        contentComponent.reset(content);
                        if (contentComponent)
                            addAndMakeVisible(contentComponent.get());

                        followSceneToggle.setButtonText("Follow Scene");
                        followSourceVolumeToggle.setButtonText("Follow Source Volume/Mute");

                        inputGainLabel.setText("Input Gain", juce::dontSendNotification);
                        outputGainLabel.setText("Output Gain", juce::dontSendNotification);
                        outputDelayLabel.setText("Output Delay", juce::dontSendNotification);

                        inputGainSlider.setRange(-30.0, 30.0, 0.1);
                        outputGainSlider.setRange(-30.0, 30.0, 0.1);
                        outputDelaySlider.setRange(0.0, 10000.0, 0.1);
                        inputGainSlider.setTextValueSuffix(" dB");
                        outputGainSlider.setTextValueSuffix(" dB");
                        outputDelaySlider.setTextValueSuffix(" ms");
                        inputGainSlider.setNumDecimalPlacesToDisplay(1);
                        outputGainSlider.setNumDecimalPlacesToDisplay(1);
                        outputDelaySlider.setNumDecimalPlacesToDisplay(1);

                        followSceneToggle.setToggleState(plugin.getFollowScene(), juce::dontSendNotification);
                        followSourceVolumeToggle.setToggleState(
                            plugin.getFollowSourceVolume(),
                            juce::dontSendNotification
                        );
                        inputGainSlider.setValue(plugin.getInputGainDb(), juce::dontSendNotification);
                        outputGainSlider.setValue(plugin.getOutputGainDb(), juce::dontSendNotification);
                        outputDelaySlider.setValue(plugin.getOutputDelayMs(), juce::dontSendNotification);

                        followSceneToggle.onClick = [this]()
                        {
                            plugin.setFollowScene(followSceneToggle.getToggleState());
                        };

                        followSourceVolumeToggle.onClick = [this]()
                        {
                            plugin.setFollowSourceVolume(followSourceVolumeToggle.getToggleState());
                        };

                        inputGainSlider.onValueChange = [this]()
                        {
                            plugin.setInputGainDb((float)inputGainSlider.getValue());
                        };

                        outputGainSlider.onValueChange = [this]()
                        {
                            plugin.setOutputGainDb((float)outputGainSlider.getValue());
                        };

                        outputDelaySlider.onValueChange = [this]()
                        {
                            plugin.setOutputDelayMs((float)outputDelaySlider.getValue());
                        };

                        addAndMakeVisible(followSceneToggle);
                        addAndMakeVisible(followSourceVolumeToggle);
                        addAndMakeVisible(inputGainLabel);
                        addAndMakeVisible(outputGainLabel);
                        addAndMakeVisible(outputDelayLabel);
                        addAndMakeVisible(inputGainSlider);
                        addAndMakeVisible(outputGainSlider);
                        addAndMakeVisible(outputDelaySlider);

                        setSize(900, 800);
                    }

                    void resized() override
                    {
                        auto bounds = getLocalBounds().reduced(8);
                        auto controlArea = bounds.removeFromTop(132);

                        auto togglesRow = controlArea.removeFromTop(28);
                        followSceneToggle.setBounds(togglesRow.removeFromLeft(220));
                        followSourceVolumeToggle.setBounds(togglesRow.removeFromLeft(320));

                        auto gainsRow = controlArea.removeFromTop(56);
                        inputGainLabel.setBounds(gainsRow.removeFromLeft(90));
                        inputGainSlider.setBounds(gainsRow.removeFromLeft(300).reduced(0, 8));
                        gainsRow.removeFromLeft(18);
                        outputGainLabel.setBounds(gainsRow.removeFromLeft(90));
                        outputGainSlider.setBounds(gainsRow.removeFromLeft(300).reduced(0, 8));

                        auto delayRow = controlArea.removeFromTop(56);
                        outputDelayLabel.setBounds(delayRow.removeFromLeft(90));
                        outputDelaySlider.setBounds(delayRow.removeFromLeft(300).reduced(0, 8));

                        if (contentComponent)
                            contentComponent->setBounds(bounds);
                    }

                private:
                    DeviceIo2Plugin& plugin;
                    std::unique_ptr<juce::Component> contentComponent;
                    juce::ToggleButton followSceneToggle;
                    juce::ToggleButton followSourceVolumeToggle;
                    juce::Label inputGainLabel;
                    juce::Label outputGainLabel;
                    juce::Label outputDelayLabel;
                    juce::Slider inputGainSlider;
                    juce::Slider outputGainSlider;
                    juce::Slider outputDelaySlider;

                    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceIo2Editor)
                };

                return new DeviceIo2Editor(*this, settingsComponent);
            }
        }
        return nullptr;
    }

    bool hasEditor() const override
    {
        return true;
    }

    void setParentSourceUuid(const std::string& uuid)
    {
        parentSourceUuid = uuid;
        hasDesiredBypassLatched = false;
        desiredBypassLatched = true;
    }

    void setFollowSourceVolume(bool shouldFollow)
    {
        followSourceVolume.store(shouldFollow, std::memory_order_release);
    }

    bool getFollowSourceVolume() const
    {
        return followSourceVolume.load(std::memory_order_acquire);
    }

    void setFollowScene(bool shouldFollow)
    {
        followScene.store(shouldFollow, std::memory_order_release);
    }

    bool getFollowScene() const
    {
        return followScene.load(std::memory_order_acquire);
    }

    void setInputGainDb(float gainDb)
    {
        inputGainDb.store(gainDb, std::memory_order_release);
    }

    float getInputGainDb() const
    {
        return inputGainDb.load(std::memory_order_acquire);
    }

    void setOutputGainDb(float gainDb)
    {
        outputGainDb.store(gainDb, std::memory_order_release);
    }

    float getOutputGainDb() const
    {
        return outputGainDb.load(std::memory_order_acquire);
    }

    void setOutputDelayMs(float delayMs)
    {
        outputDelayMs.store(delayMs, std::memory_order_release);
        if (deviceIo2)
            deviceIo2->setOutputDelay(delayMs);
    }

    float getOutputDelayMs() const
    {
        return outputDelayMs.load(std::memory_order_acquire);
    }

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        destData.reset();

        std::string state;
        if (!pendingSerializedState.empty())
            state = pendingSerializedState;

        if (state.empty() && deviceIo2)
            deviceIo2->getState(state);

        juce::XmlElement root("DeviceIo2PluginState");
        root.setAttribute("followSourceVolume", getFollowSourceVolume());
        root.setAttribute("followScene", getFollowScene());
        root.setAttribute("inputGainDb", getInputGainDb());
        root.setAttribute("outputGainDb", getOutputGainDb());
        root.setAttribute("outputDelayMs", getOutputDelayMs());

        if (!state.empty())
        {
            juce::MemoryBlock stateData(state.data(), state.size());
            root.setAttribute("deviceIo2StateBase64", stateData.toBase64Encoding());
        }

        auto xmlState = root.toString();
        destData.replaceAll(xmlState.toRawUTF8(), (size_t)xmlState.getNumBytesAsUTF8());
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        if (sizeInBytes <= 0 || data == nullptr)
        {
            pendingSerializedState.clear();
            return;
        }

        std::string serializedState(static_cast<const char*>(data), static_cast<size_t>(sizeInBytes));
        juce::XmlDocument xmlDocument(serializedState);
        auto root = xmlDocument.getDocumentElement();

        if (root != nullptr && root->hasTagName("DeviceIo2PluginState"))
        {
            setFollowSourceVolume(root->getBoolAttribute("followSourceVolume", getFollowSourceVolume()));
            setFollowScene(root->getBoolAttribute("followScene", getFollowScene()));
            setInputGainDb((float)root->getDoubleAttribute("inputGainDb", getInputGainDb()));
            setOutputGainDb((float)root->getDoubleAttribute("outputGainDb", getOutputGainDb()));
            setOutputDelayMs((float)root->getDoubleAttribute("outputDelayMs", getOutputDelayMs()));

            auto encodedState = root->getStringAttribute("deviceIo2StateBase64");
            if (encodedState.isNotEmpty())
            {
                juce::MemoryBlock stateData;
                if (stateData.fromBase64Encoding(encodedState))
                    pendingSerializedState.assign(static_cast<char*>(stateData.getData()), stateData.getSize());
                else
                    pendingSerializedState.clear();
            }
            else
            {
                pendingSerializedState.clear();
            }
        }
        else
        {
            // Backward compatibility with legacy state format: raw DeviceIo2 state blob.
            pendingSerializedState = serializedState;
        }

        if (deviceIo2)
            applyPendingStateIfAny();
    }

private:
    void timerCallback() override
    {
        atk::DeviceIo2::WrapperControlState controlState;
        controlState.followSourceVolume = followSourceVolume.load(std::memory_order_acquire);
        controlState.followScene = followScene.load(std::memory_order_acquire);
        controlState.inputGainDb = inputGainDb.load(std::memory_order_acquire);
        controlState.outputGainDb = outputGainDb.load(std::memory_order_acquire);

        if (controlState.followScene)
        {
            controlState.fadeTimeSeconds = atk::deviceIo2ObsControl::getCurrentTransitionFadeSeconds();

            if (parentSourceUuid.empty())
            {
                hasDesiredBypassLatched = true;
                desiredBypassLatched = false;
            }
            else
            {
                bool desiredBypass = true;
                if (atk::deviceIo2ObsControl::resolveDesiredBypass(parentSourceUuid, desiredBypass))
                {
                    hasDesiredBypassLatched = true;
                    desiredBypassLatched = desiredBypass;
                }
            }

            controlState.hasDesiredBypass = hasDesiredBypassLatched;
            controlState.desiredBypass = desiredBypassLatched;
        }
        else
        {
            hasDesiredBypassLatched = true;
            desiredBypassLatched = false;
            controlState.hasDesiredBypass = true;
            controlState.desiredBypass = false;
            controlState.fadeTimeSeconds = 0.5;
        }

        if (controlState.followSourceVolume)
        {
            atk::deviceIo2ObsControl::SourceLevelState sourceLevelState;
            if (atk::deviceIo2ObsControl::readSourceLevelState(parentSourceUuid, sourceLevelState))
            {
                controlState.sourceVolume = sourceLevelState.sourceVolume;
                controlState.sourceMuted = sourceLevelState.sourceMuted;
            }
            else
            {
                controlState.sourceVolume = 1.0f;
                controlState.sourceMuted = false;
            }
        }
        else
        {
            controlState.sourceVolume = 1.0f;
            controlState.sourceMuted = false;
        }

        if (deviceIo2)
        {
            deviceIo2->setOutputDelay(getOutputDelayMs());
            deviceIo2->setWrapperControlState(controlState);
        }
        else
            pendingControlState = controlState;
    }

    void ensureDeviceIo2Initialized()
    {
        if (deviceIo2)
            return;

        deviceIo2 = std::make_unique<atk::DeviceIo2>();
        deviceIo2->setOutputDelay(getOutputDelayMs());
        deviceIo2->setWrapperControlState(pendingControlState);
    }

    void applyPendingStateIfAny()
    {
        std::string state;
        if (!deviceIo2 || pendingSerializedState.empty())
            return;

        state = pendingSerializedState;
        pendingSerializedState.clear();

        deviceIo2->setState(state);
    }

    std::unique_ptr<atk::DeviceIo2> deviceIo2;
    std::string pendingSerializedState;
    std::string parentSourceUuid;
    std::atomic<bool> followSourceVolume{false};
    std::atomic<bool> followScene{true};
    std::atomic<float> inputGainDb{0.0f};
    std::atomic<float> outputGainDb{0.0f};
    std::atomic<float> outputDelayMs{0.0f};
    atk::DeviceIo2::WrapperControlState pendingControlState;
    bool hasDesiredBypassLatched = false;
    bool desiredBypassLatched = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceIo2Plugin)
};
