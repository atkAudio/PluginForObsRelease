#pragma once

#include "../FifoBuffer2.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <string>
#include <util/platform.h>
#include <vector>

constexpr auto PROPERTY_NAME = "mixes";
constexpr auto CHILD_NAME = "SelectedMixes";

class ObsOutputAudioProcessor : public juce::AudioProcessor
{
public:
    ObsOutputAudioProcessor()
        : apvts(*this, nullptr, "Parameters", {})
    {
        auto idNumber = 1 + numInstances.fetch_add(1, std::memory_order_relaxed);
        auto id = std::string("atkaudio_ph2helper");
        auto name = "Ph2Out" + std::to_string(idNumber);

        privateSource = obs_get_source_by_name(name.c_str());
        if (!privateSource)
        {
            privateSource = obs_source_create(id.c_str(), name.c_str(), nullptr, nullptr);

            auto* sceneSource = obs_frontend_get_current_scene();
            auto* scene = obs_scene_from_source(sceneSource);
            obs_scene_add(scene, privateSource);

            obs_source_set_audio_active(privateSource, true);
            obs_source_set_enabled(privateSource, true);
        }
    }

    ~ObsOutputAudioProcessor() override
    {
        numInstances.fetch_sub(1, std::memory_order_acquire);
        if (privateSource)
        {
            obs_source_release(privateSource);
            obs_source_remove(privateSource);
        }
    }

    const juce::String getName() const override
    {
        return "OBS Output";
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        for (int i = 0; i < MAX_AUDIO_CHANNELS; ++i)
            if (i < buffer.getNumChannels())
                audioSourceData.data[i] = (const uint8_t*)buffer.getReadPointer(i);
            else
                audioSourceData.data[i] = nullptr;

        audioSourceData.frames = static_cast<uint32_t>(buffer.getNumSamples());
        audioSourceData.speakers = getMainBusNumInputChannels() <= MAX_AUDIO_CHANNELS
                                     ? static_cast<enum speaker_layout>(getMainBusNumInputChannels())
                                     : SPEAKERS_UNKNOWN;
        audioSourceData.format = AUDIO_FORMAT_FLOAT_PLANAR;
        audioSourceData.samples_per_sec = static_cast<uint32_t>(getSampleRate());
        audioSourceData.timestamp = os_gettime_ns();
        obs_source_output_audio(privateSource, &audioSourceData);
    }

    void releaseResources() override
    {
    }

    juce::AudioProcessorEditor* createEditor() override;

    bool hasEditor() const override
    {
        return true;
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
        return {};
    }

    void changeProgramName(int, const juce::String&) override
    {
    }

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        auto state = apvts.copyState();
        std::unique_ptr<juce::XmlElement> xml(state.createXml());
        copyXmlToBinary(*xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
        if (xmlState.get() != nullptr)
            if (xmlState->hasTagName(apvts.state.getType()))
                apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
    }

    bool acceptsMidi() const override
    {
        return false;
    }

    bool producesMidi() const override
    {
        return false;
    }

    auto& getApvts()
    {
        return apvts;
    }

private:
    static inline std::atomic_int numInstances{0};
    juce::AudioProcessorValueTreeState apvts;

    obs_source_t* privateSource = nullptr;
    obs_data_t* sourceSettings = nullptr;
    obs_source_audio audioSourceData;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ObsOutputAudioProcessor)
};

class ObsOutputAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    ObsOutputAudioProcessorEditor(ObsOutputAudioProcessor& p)
        : juce::AudioProcessorEditor(&p)
        , processor(p)
    {
        setSize(300, 200);
        setResizable(true, true);
        setResizeLimits(200, 100, 300, 600);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
    }

private:
    ObsOutputAudioProcessor& processor;
    juce::Array<juce::String> items;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ObsOutputAudioProcessorEditor)
};

// Implementation of createEditor
inline juce::AudioProcessorEditor* ObsOutputAudioProcessor::createEditor()
{
    return new ObsOutputAudioProcessorEditor(*this);
}