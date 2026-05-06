#pragma once

#include <atkaudio/DeviceIo2/DeviceIo2.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <string>

class DeviceIo2Plugin final : public juce::AudioProcessor
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
    }

    ~DeviceIo2Plugin() override
    {
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

        if (deviceIo2->isBypassed())
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
                    DeviceIo2Editor(juce::AudioProcessor& p, juce::Component* content)
                        : AudioProcessorEditor(p)
                    {
                        // Take ownership of the content component
                        contentComponent.reset(content);
                        if (contentComponent)
                        {
                            addAndMakeVisible(contentComponent.get());
                            setSize(900, 700);
                        }
                    }

                    void resized() override
                    {
                        if (contentComponent)
                            contentComponent->setBounds(getLocalBounds());
                    }

                private:
                    std::unique_ptr<juce::Component> contentComponent;

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

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        destData.reset();

        std::string state;
        if (!pendingSerializedState.empty())
            state = pendingSerializedState;

        if (!state.empty())
        {
            // Authoritative until applied to DeviceIo2 instance.
            destData.replaceAll(state.data(), state.size());
            return;
        }

        if (deviceIo2)
            deviceIo2->getState(state);

        if (!state.empty())
            destData.replaceAll(state.data(), state.size());
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        if (sizeInBytes <= 0 || data == nullptr)
        {
            pendingSerializedState.clear();
            return;
        }

        pendingSerializedState.assign(static_cast<const char*>(data), static_cast<size_t>(sizeInBytes));

        if (deviceIo2)
            applyPendingStateIfAny();
    }

private:
    void ensureDeviceIo2Initialized()
    {
        if (deviceIo2)
            return;

        deviceIo2 = std::make_unique<atk::DeviceIo2>();
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceIo2Plugin)
};
