#include "DelayLinePlugin.h"

//==============================================================================
DelayLinePlugin::DelayLinePlugin()
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo())
              .withOutput("Output", juce::AudioChannelSet::stereo())
      )
{
    apvts = std::make_unique<juce::AudioProcessorValueTreeState>(*this, nullptr, "state", createParameterLayout());
    delayMsValue = apvts->getRawParameterValue("delay");
}

void DelayLinePlugin::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec{
        sampleRate,
        static_cast<juce::uint32>(samplesPerBlock),
        static_cast<juce::uint32>(getTotalNumOutputChannels())
    };

    maxDelaySamples = static_cast<int>(sampleRate * 10.0);
    delayLine.setMaximumDelayInSamples(maxDelaySamples);
    delayLine.prepare(spec);

    const auto delayMs = delayMsValue->load(std::memory_order_relaxed);
    const auto delaySamples = static_cast<float>(delayMs * sampleRate * 0.001);
    delaySmoothed.reset(sampleRate, 0.4);
    delaySmoothed.setCurrentAndTargetValue(delaySamples);
}

void DelayLinePlugin::releaseResources()
{
    delayLine.reset();
}

void DelayLinePlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto delayMs = delayMsValue->load(std::memory_order_relaxed);
    const auto targetDelaySamples =
        juce::jlimit(0.0f, static_cast<float>(maxDelaySamples), static_cast<float>(delayMs * getSampleRate() * 0.001));

    delaySmoothed.setTargetValue(targetDelaySamples);

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (delaySmoothed.isSmoothing())
    {
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto currentDelay =
                juce::jlimit(0.0f, static_cast<float>(maxDelaySamples), delaySmoothed.getNextValue());
            delayLine.setDelay(currentDelay);
            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto* data = buffer.getWritePointer(channel);
                delayLine.pushSample(channel, data[sample]);
                data[sample] = delayLine.popSample(channel);
            }
        }
    }
    else
    {
        delayLine.setDelay(targetDelaySamples);
        for (int sample = 0; sample < numSamples; ++sample)
        {
            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto* data = buffer.getWritePointer(channel);
                delayLine.pushSample(channel, data[sample]);
                data[sample] = delayLine.popSample(channel);
            }
        }
    }
}

juce::AudioProcessorEditor* DelayLinePlugin::createEditor()
{
    return new DelayLineEditor(*this);
}

void DelayLinePlugin::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts->copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void DelayLinePlugin::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = std::unique_ptr<juce::XmlElement>(getXmlFromBinary(data, sizeInBytes)))
        apvts->replaceState(juce::ValueTree::fromXml(*xml));
}

bool DelayLinePlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& mainInLayout = layouts.getChannelSet(true, 0);
    const auto& mainOutLayout = layouts.getChannelSet(false, 0);
    return (mainInLayout == mainOutLayout && (!mainInLayout.isDisabled()));
}

juce::AudioProcessorValueTreeState::ParameterLayout DelayLinePlugin::createParameterLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    auto delayRange = NormalisableRange<float>(0.0f, 10000.0f, 0.1f);
    delayRange.setSkewForCentre(1000.0f);
    params.push_back(
        std::make_unique<AudioParameterFloat>(
            ParameterID{"delay", 1},
            "Delay (ms)",
            delayRange,
            0.0f,
            AudioParameterFloatAttributes()
                .withStringFromValueFunction([](float value, int) { return String(value, 1) + " ms"; })
                .withValueFromStringFunction([](const String& text)
                                             { return text.trimCharactersAtEnd(" ms").getFloatValue(); })
        )
    );

    return {params.begin(), params.end()};
}

//==============================================================================
DelayLineEditor::DelayLineEditor(DelayLinePlugin& p)
    : AudioProcessorEditor(p)
    , processor(p)
    , delayAttachment(processor.getApvts(), "delay", delaySlider)
{
    setSize(300, 60);

    delayLabel.setText("Delay (ms):", juce::dontSendNotification);
    delayLabel.attachToComponent(&delaySlider, true);
    addAndMakeVisible(delayLabel);

    delaySlider.setSliderStyle(juce::Slider::LinearHorizontal);
    delaySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    addAndMakeVisible(delaySlider);
}

void DelayLineEditor::resized()
{
    auto area = getLocalBounds().reduced(8);
    auto sliderArea = area.removeFromTop(24);
    delaySlider.setBounds(sliderArea.withTrimmedLeft(80));
}
