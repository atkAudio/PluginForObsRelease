#pragma once

#include "../../FifoBuffer2.h"

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
        : juce::AudioProcessor(
              juce::AudioProcessor::BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
          )
        , apvts(*this, nullptr, "Parameters", {})
    {
        // Pairing with helper source is now done in setStateInformation
        // to support UUID-based tracking
    }

    ~ObsOutputAudioProcessor() override
    {
        if (privateSource)
        {
            obs_source_release(privateSource);
            privateSource = nullptr;
        }
    }

    const juce::String getName() const override
    {
        return "OBS Output";
    }

    void prepareToPlay(double, int) override
    {
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        // Ensure we have a helper source before processing
        if (!privateSource && !sourceCreationScheduled)
        {
            DBG("ObsOutput: Creating helper source (first processBlock)");
            sourceCreationScheduled = true;
            createNewHelperSource();
        }

        if (!privateSource)
            return; // No source available yet

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

        // Add helper source UUID to state
        if (privateSource)
        {
            const char* uuid = obs_source_get_uuid(privateSource);
            if (uuid && uuid[0] != '\0')
                xml->setAttribute("helperSourceUuid", juce::String(uuid));
        }

        copyXmlToBinary(*xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
        if (xmlState.get() != nullptr)
        {
            if (xmlState->hasTagName(apvts.state.getType()))
                apvts.replaceState(juce::ValueTree::fromXml(*xmlState));

            // Look for helper source by UUID
            if (xmlState->hasAttribute("helperSourceUuid"))
            {
                juce::String uuidStr = xmlState->getStringAttribute("helperSourceUuid");
                if (uuidStr.isNotEmpty())
                    pairWithHelperByUuid(uuidStr.toStdString());
            }
            else if (!privateSource && !sourceCreationScheduled)
            {
                DBG("ObsOutput: Creating helper source (no UUID in state)");
                sourceCreationScheduled = true;
                createNewHelperSource();
            }
        }
        else if (!privateSource && !sourceCreationScheduled)
        {
            DBG("ObsOutput: Creating helper source (no state)");
            sourceCreationScheduled = true;
            createNewHelperSource();
        }
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
    juce::AudioProcessorValueTreeState apvts;

    obs_source_t* privateSource = nullptr;
    obs_source_audio audioSourceData;
    bool sourceCreationScheduled = false; // Prevent duplicate scheduling

    void pairWithHelperByUuid(const std::string& uuid)
    {
        // Release any existing source
        if (privateSource)
        {
            obs_source_release(privateSource);
            privateSource = nullptr;
        }

        // Search for source with matching UUID
        struct FindContext
        {
            const std::string* targetUuid;
            obs_source_t* foundSource;
        } context{&uuid, nullptr};

        obs_enum_sources(
            [](void* param, obs_source_t* source) -> bool
            {
                auto* ctx = static_cast<FindContext*>(param);
                const char* sourceUuid = obs_source_get_uuid(source);

                if (sourceUuid && *ctx->targetUuid == sourceUuid)
                {
                    // Get a new reference to the source (obs_enum_sources doesn't add a ref)
                    ctx->foundSource = obs_source_get_ref(source);
                    return false; // stop enumeration
                }
                return true; // continue enumeration
            },
            &context
        );

        if (context.foundSource)
        {
            privateSource = context.foundSource;
            DBG("ObsOutput: Paired with existing helper source via UUID");
        }
        else if (!sourceCreationScheduled)
        {
            DBG("ObsOutput: Creating helper source (UUID not found)");
            sourceCreationScheduled = true;
            createNewHelperSource();
        }
    }

    void createNewHelperSource()
    {
        privateSource = obs_source_create("atkaudio_ph2helper", "Ph2Out", nullptr, nullptr);

        if (privateSource)
        {
            obs_source_set_audio_active(privateSource, true);
            obs_source_set_enabled(privateSource, true);

            // Add to current scene
            auto* currentScene = obs_frontend_get_current_scene();
            if (currentScene)
            {
                auto* sceneSource = obs_scene_from_source(currentScene);
                if (sceneSource)
                {
                    if (obs_scene_add(sceneSource, privateSource))
                        DBG("ObsOutput: Helper source created and added to scene");
                }
                obs_source_release(currentScene);
            }
        }
        else
        {
            DBG("ObsOutput: Failed to create helper source!");
        }

        sourceCreationScheduled = false;
    }
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ObsOutputAudioProcessorEditor)
};

// Implementation of createEditor
inline juce::AudioProcessorEditor* ObsOutputAudioProcessor::createEditor()
{
    return new ObsOutputAudioProcessorEditor(*this);
}