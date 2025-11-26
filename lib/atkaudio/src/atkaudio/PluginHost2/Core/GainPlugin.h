#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

//==============================================================================
class GainPlugin final
    : public juce::AudioProcessor
    , public juce::Timer
{
public:
    //==============================================================================
    GainPlugin()
        : AudioProcessor(
              BusesProperties()
                  .withInput("Input", juce::AudioChannelSet::stereo())
                  .withOutput("Output", juce::AudioChannelSet::stereo())
          )
    {
        using namespace juce;
        apvts = std::make_unique<AudioProcessorValueTreeState>(*this, nullptr, "state", createParameterLayout());
        gainValue = apvts->getRawParameterValue("gain");
        gain2Value = apvts->getRawParameterValue("gain2");
        midiEnabled = apvts->getRawParameterValue("midi");
        midiChannel = apvts->getRawParameterValue("ch");
        midiCc = apvts->getRawParameterValue("cc");
        midiLearn = apvts->getRawParameterValue("learn");

        gainParam = apvts->getParameter("gain");
        channelParam = apvts->getParameter("ch");
        ccParam = apvts->getParameter("cc");
        midiEnabledParam = apvts->getParameter("midi");

        startTimerHz(30); // Start the timer to process MIDI messages
    }

    void timerCallback() override
    {
        if (midiEnabled->load(std::memory_order_acquire) > 0.5f)
        {
            // Only update gain if we've received a MIDI message
            if (gainUpdated.load(std::memory_order_acquire))
            {
                auto gain = toUiGain.load(std::memory_order_acquire);
                gain = gainParam->getNormalisableRange().convertTo0to1(gain);
                gainParam->setValueNotifyingHost(gain);
                gainUpdated.store(false, std::memory_order_release);
            }
        }

        if (midiLearn->load(std::memory_order_acquire) > 0.5f)
        {
            // Check if we've captured new values
            if (learnCaptured.load(std::memory_order_acquire))
            {
                auto cc = toUiCc.load(std::memory_order_acquire);
                auto ch = toUiChannel.load(std::memory_order_acquire);

                cc = ccParam->getNormalisableRange().convertTo0to1(cc);
                ch = channelParam->getNormalisableRange().convertTo0to1(ch);

                ccParam->setValueNotifyingHost(cc);
                channelParam->setValueNotifyingHost(ch);

                // Enable MIDI control
                midiEnabledParam->setValueNotifyingHost(1.0f);

                DBG("GainPlugin MIDI Learn complete: ch=" << toUiChannel.load() << " cc=" << toUiCc.load());

                // Turn off learn mode
                auto* learnParam = apvts->getParameter("learn");
                learnParam->setValueNotifyingHost(0.0f);

                // Clear the capture flag
                learnCaptured.store(false, std::memory_order_release);
            }
        }
    }

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        juce::ignoreUnused(sampleRate, samplesPerBlock);
        gainValueSmoothed.reset(sampleRate, 0.05f);  // Smooth the gain value with a time constant of 50ms
        gain2ValueSmoothed.reset(sampleRate, 0.05f); // Smooth the gain2 value with a time constant of 50ms
    }

    void releaseResources() override
    {
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiBuffer) override
    {
        auto gain = gainValue->load(std::memory_order_acquire);
        auto gain2Db = gain2Value->load(std::memory_order_acquire);
        auto gain2Linear = juce::Decibels::decibelsToGain(gain2Db);

        // Debug: Check if we're receiving any MIDI
        if (!midiBuffer.isEmpty())
            DBG("GainPlugin received " << midiBuffer.getNumEvents() << " MIDI events");

        if (midiEnabled->load(std::memory_order_acquire) > 0.5f)
        {
            for (const auto& metadata : midiBuffer)
            {
                const juce::MidiMessage message(metadata.data, metadata.numBytes, metadata.samplePosition);
                if (message.isController())
                {
                    auto expectedChannel = static_cast<int>(midiChannel->load(std::memory_order_acquire));
                    auto expectedCc = static_cast<int>(midiCc->load(std::memory_order_acquire));

                    DBG("GainPlugin MIDI CC: ch="
                        << message.getChannel()
                        << " cc="
                        << message.getControllerNumber()
                        << " value="
                        << message.getControllerValue()
                        << " | Expected: ch="
                        << expectedChannel
                        << " cc="
                        << expectedCc);

                    if (message.getChannel() == expectedChannel && message.getControllerNumber() == expectedCc)
                    {
                        // Convert MIDI CC to gain using the parameter's normalisable range
                        // The gain parameter already has skew set to 0.125 for cubic response
                        auto faderPos = message.getControllerValue() / 127.0f;

                        gain = gainParam->getNormalisableRange().convertFrom0to1(faderPos);
                        toUiGain.store(gain, std::memory_order_release);
                        gainUpdated.store(true, std::memory_order_release);
                        DBG("GainPlugin updated gain to: " << gain);
                    }
                }
            }
        }

        auto initialGain = gainValueSmoothed.getCurrentValue();
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            gainValueSmoothed.setTargetValue(gain);
            gain2ValueSmoothed.setTargetValue(gain2Linear);
            auto* readPtr = buffer.getReadPointer(channel);
            auto* writePtr = buffer.getWritePointer(channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                auto currentGain = gainValueSmoothed.getNextValue();
                auto currentGain2 = gain2ValueSmoothed.getNextValue();
                writePtr[sample] = readPtr[sample] * currentGain * currentGain2;
            }
        }

        if (midiLearn->load(std::memory_order_acquire) > 0.5f)
        {
            DBG("GainPlugin: Learn mode active, buffer has " << midiBuffer.getNumEvents() << " events");

            for (const auto& metadata : midiBuffer)
            {
                const juce::MidiMessage message(metadata.data, metadata.numBytes, metadata.samplePosition);

                DBG("GainPlugin: Learn mode checking message - isController="
                    << (message.isController() ? "yes" : "no")
                    << " isNoteOn="
                    << (message.isNoteOn() ? "yes" : "no"));

                if (message.isController())
                {
                    DBG("GainPlugin MIDI Learn captured: ch="
                        << message.getChannel()
                        << " cc="
                        << message.getControllerNumber());

                    toUiChannel.store(message.getChannel(), std::memory_order_release);
                    toUiCc.store(message.getControllerNumber(), std::memory_order_release);
                    learnCaptured.store(true, std::memory_order_release);
                    break;
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
        gainRange.setSkewForCentre(0.125f); // Match cubic curve: 0.5^3 = 0.125

        auto channelRange = NormalisableRange<float>(0.0f, 16.0f, 1.0f, 1.0f);
        auto ccRange = NormalisableRange<float>(0.0f, 128.0f, 1.0f, 1.0f);

        params.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"gain", 1}, "Gain", gainRange, 1.0f));

        params.push_back(std::make_unique<AudioParameterBool>(ParameterID{"midi", 1}, "MIDI", false));

        params.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"ch", 1}, "Channel", channelRange, 1.0f));

        params.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"cc", 1}, "CC", ccRange, 1.0f));

        params.push_back(std::make_unique<AudioParameterBool>(ParameterID{"learn", 1}, "Learn", false));

        // Gain2 parameter with -30 to +30 dB range
        auto gain2Range = NormalisableRange<float>(-30.0f, 30.0f, 0.1f, 1.0f);
        params.push_back(
            std::make_unique<AudioParameterFloat>(ParameterID{"gain2", 1}, "Gainsborough", gain2Range, 0.0f)
        );

        return {params.begin(), params.end()};
    }

    std::unique_ptr<juce::AudioProcessorValueTreeState> apvts;
    std::atomic<float>* gainValue = nullptr;
    std::atomic<float>* gain2Value = nullptr;
    std::atomic<float>* midiEnabled = nullptr;
    std::atomic<float>* midiChannel = nullptr;
    std::atomic<float>* midiCc = nullptr;
    std::atomic<float>* midiLearn = nullptr;

    juce::LinearSmoothedValue<float> gainValueSmoothed;
    juce::LinearSmoothedValue<float> gain2ValueSmoothed;

    juce::RangedAudioParameter* gainParam = nullptr;
    juce::RangedAudioParameter* channelParam = nullptr;
    juce::RangedAudioParameter* ccParam = nullptr;
    juce::RangedAudioParameter* midiEnabledParam = nullptr;

    std::atomic<float> toUiGain = 0.0f;
    std::atomic<float> toUiChannel = 0.0f;
    std::atomic<float> toUiCc = 0.0f;
    std::atomic<bool> learnCaptured = false;
    std::atomic<bool> gainUpdated = false;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GainPlugin)
};
