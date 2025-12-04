#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

class DelayLinePlugin;

class DelayLineEditor final : public juce::AudioProcessorEditor
{
public:
    explicit DelayLineEditor(DelayLinePlugin& p);
    void resized() override;

private:
    DelayLinePlugin& processor;

    juce::Label delayLabel;
    juce::Slider delaySlider;
    juce::AudioProcessorValueTreeState::SliderAttachment delayAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DelayLineEditor)
};

class DelayLinePlugin final : public juce::AudioProcessor
{
public:
    DelayLinePlugin();

    juce::AudioProcessorValueTreeState& getApvts()
    {
        return *apvts;
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;

    bool hasEditor() const override
    {
        return true;
    }

    const juce::String getName() const override
    {
        return "Delay Line";
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
        return 10.0;
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
        return "None";
    }

    void changeProgramName(int, const juce::String&) override
    {
    }

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::unique_ptr<juce::AudioProcessorValueTreeState> apvts;
    std::atomic<float>* delayMsValue = nullptr;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine;
    juce::LinearSmoothedValue<float> delaySmoothed;
    int maxDelaySamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DelayLinePlugin)
};
