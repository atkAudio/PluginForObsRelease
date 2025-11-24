#pragma once

#include <atkaudio/DeviceIo2/DeviceIo2.h>
#include <juce_audio_utils/juce_audio_utils.h>

//==============================================================================
/**
 * DeviceIo2Plugin - An internal plugin wrapper for DeviceIo2
 *
 * This wraps the DeviceIo2 audio module as a JUCE AudioProcessor so it can be
 * embedded in the PluginHost2 graph as an internal effect/processor.
 *
 * It acts as a bridge between the graph's processing and the DeviceIo2's
 * device I/O and routing capabilities.
 */
class DeviceIo2Plugin final : public juce::AudioProcessor
{
public:
    //==============================================================================
    DeviceIo2Plugin()
        : AudioProcessor(
              BusesProperties()
                  .withInput("Input", juce::AudioChannelSet::stereo())
                  .withOutput("Output", juce::AudioChannelSet::stereo())
          )
    {
        deviceIo2 = std::make_unique<atk::DeviceIo2>();
        // Note: DeviceIo2 internal routing matrix is initialized with default diagonal routing
        // It will auto-resize based on the actual OBS channel count during processing
    }

    ~DeviceIo2Plugin() override
    {
        deviceIo2.reset();
    }

    //==============================================================================
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

    //==============================================================================
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

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        juce::ignoreUnused(sampleRate, samplesPerBlock);
        // DeviceIo2 handles its own preparation internally during process()
    }

    void releaseResources() override
    {
        // DeviceIo2 handles its own resource management
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
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

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override
    {
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

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        if (!deviceIo2)
            return;

        std::string state;
        deviceIo2->getState(state);

        if (!state.empty())
            destData.replaceAll(state.data(), state.size());
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        if (!deviceIo2 || sizeInBytes <= 0)
            return;

        std::string state(static_cast<const char*>(data), sizeInBytes);
        deviceIo2->setState(state);
    }

private:
    //==============================================================================
    std::unique_ptr<atk::DeviceIo2> deviceIo2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceIo2Plugin)
};
