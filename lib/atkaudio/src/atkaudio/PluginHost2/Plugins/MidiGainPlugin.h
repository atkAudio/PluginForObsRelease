#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

//==============================================================================
class MidiGainPlugin final
    : public juce::AudioProcessor
    , public juce::Timer
{
public:
    //==============================================================================
    MidiGainPlugin()
        : AudioProcessor(
              BusesProperties()
                  .withInput("Input", juce::AudioChannelSet::stereo())
                  .withOutput("Output", juce::AudioChannelSet::stereo())
          )
    {
        using namespace juce;
        apvts = std::make_unique<AudioProcessorValueTreeState>(*this, nullptr, "state", createParameterLayout());
        gainValue = apvts->getRawParameterValue("gain");
        midiEnabled = apvts->getRawParameterValue("midi");
        midiChannel = apvts->getRawParameterValue("ch");
        midiCc = apvts->getRawParameterValue("cc");
        midiLearn = apvts->getRawParameterValue("learn");

        gainParam = apvts->getParameter("gain");
        channelParam = apvts->getParameter("ch");
        ccParam = apvts->getParameter("cc");

        startTimerHz(30); // Start the timer to process MIDI messages
    }

    void timerCallback() override
    {
        if (midiEnabled->load(std::memory_order_acquire) > 0.5f)
        {
            auto gain = toUiGain.load(std::memory_order_acquire);
            gain = gainParam->getNormalisableRange().convertTo0to1(gain);
            gainParam->setValueNotifyingHost(gain);
        }

        if (midiLearn->load(std::memory_order_acquire) > 0.5f)
        {
            auto cc = toUiCc.load(std::memory_order_acquire);
            auto ch = toUiChannel.load(std::memory_order_acquire);

            cc = ccParam->getNormalisableRange().convertTo0to1(cc);
            ch = channelParam->getNormalisableRange().convertTo0to1(ch);

            ccParam->setValueNotifyingHost(cc);
            channelParam->setValueNotifyingHost(ch);
        }
    }

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        juce::ignoreUnused(sampleRate, samplesPerBlock);
        gainValueSmoothed.reset(sampleRate, 0.05f); // Smooth the gain value with a time constant of 50ms
    }

    void releaseResources() override
    {
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiBuffer) override
    {
        auto gain = gainValue->load(std::memory_order_acquire);

        if (midiEnabled->load(std::memory_order_acquire) > 0.5f)
        {
            for (const auto& event : midiBuffer)
            {
                const auto& message = event.getMessage();
                if (message.isController()
                    && message.getChannel() == static_cast<int>(midiChannel->load(std::memory_order_acquire)))
                {
                    if (message.getControllerNumber() == static_cast<int>(midiCc->load(std::memory_order_acquire)))
                    {
                        auto faderPos = message.getControllerValue() / 127.0f;
                        gain = gainParam->getNormalisableRange().convertFrom0to1(faderPos);
                        toUiGain.store(gain, std::memory_order_release);
                    }
                }
            }
        }

        auto initialGain = gainValueSmoothed.getCurrentValue();
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            gainValueSmoothed.setTargetValue(gain);
            auto* readPtr = buffer.getReadPointer(channel);
            auto* writePtr = buffer.getWritePointer(channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                writePtr[sample] = readPtr[sample] * gainValueSmoothed.getNextValue();
        }

        if (midiLearn->load(std::memory_order_acquire) > 0.5f)
        {
            for (const auto& event : midiBuffer)
            {
                const auto& message = event.getMessage();
                if (message.isController())
                {
                    auto channel = message.getChannel();
                    auto cc = message.getControllerNumber();

                    toUiChannel.store(channel, std::memory_order_release);
                    toUiCc.store(cc, std::memory_order_release);
                }
            }
        }
    }

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override
    {
        return new juce::GenericAudioProcessorEditor(*this);
    }

    bool hasEditor() const override
    {
        return true;
    }

    //==============================================================================
    const juce::String getName() const override
    {
        return "Gain Plugin";
    }

    bool acceptsMidi() const override
    {
        return true;
    }

    bool producesMidi() const override
    {
        return false;
    }

    double getTailLengthSeconds() const override
    {
        return 0;
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
        return "None";
    }

    void changeProgramName(int, const juce::String&) override
    {
    }

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        if (auto xml = apvts->copyState().createXml())
            copyXmlToBinary(*xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xml{getXmlFromBinary(data, sizeInBytes)};
        if (xml != nullptr)
            apvts->replaceState(juce::ValueTree::fromXml(*xml));

        toUiGain.store(gainValue->load(std::memory_order_acquire), std::memory_order_release);
        toUiChannel.store(midiChannel->load(std::memory_order_acquire), std::memory_order_release);
        toUiCc.store(midiCc->load(std::memory_order_acquire), std::memory_order_release);
    }

    //==============================================================================
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto& mainInLayout = layouts.getChannelSet(true, 0);
        const auto& mainOutLayout = layouts.getChannelSet(false, 0);

        return (mainInLayout == mainOutLayout && (!mainInLayout.isDisabled()));
    }

private:
    //==============================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        using namespace juce;
        std::vector<std::unique_ptr<RangedAudioParameter>> params;

        auto gainRange = NormalisableRange<float>(0.0f, 1.0f, 0.0f, 1.0f);
        gainRange.setSkewForCentre(0.25f);

        auto channelRange = NormalisableRange<float>(0.0f, 16.0f, 1.0f, 1.0f);
        auto ccRange = NormalisableRange<float>(0.0f, 128.0f, 1.0f, 1.0f);

        params.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"gain", 1}, "Gain", gainRange, 1.0f));

        params.push_back(std::make_unique<AudioParameterBool>(ParameterID{"midi", 1}, "MIDI", false));

        params.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"ch", 1}, "Channel", channelRange, 1.0f));

        params.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"cc", 1}, "CC", ccRange, 1.0f));

        params.push_back(std::make_unique<AudioParameterBool>(ParameterID{"learn", 1}, "Learn", false));

        return {params.begin(), params.end()};
    }

    std::unique_ptr<juce::AudioProcessorValueTreeState> apvts;
    std::atomic<float>* gainValue = nullptr;
    std::atomic<float>* midiEnabled = nullptr;
    std::atomic<float>* midiChannel = nullptr;
    std::atomic<float>* midiCc = nullptr;
    std::atomic<float>* midiLearn = nullptr;

    juce::LinearSmoothedValue<float> gainValueSmoothed;

    juce::RangedAudioParameter* gainParam = nullptr;
    juce::RangedAudioParameter* channelParam = nullptr;
    juce::RangedAudioParameter* ccParam = nullptr;

    std::atomic<float> toUiGain = 0.0f;
    std::atomic<float> toUiChannel = 0.0f;
    std::atomic<float> toUiCc = 0.0f;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiGainPlugin)
};
