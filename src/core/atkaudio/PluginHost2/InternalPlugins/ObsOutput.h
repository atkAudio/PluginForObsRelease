#pragma once

#include "../../FifoBuffer2.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <mutex>
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
        obs_frontend_add_event_callback(frontendEventCallback, this);
    }

    ~ObsOutputAudioProcessor() override
    {
        obs_frontend_remove_event_callback(frontendEventCallback, this);
        releaseHelperSource();
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
        if (!sourceConnected.load(std::memory_order_acquire))
            scheduleHelperSourceConnection();

        std::lock_guard<std::mutex> lock(processingMutex);

        if (!privateSource)
            return;

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
        releaseHelperSource();
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
        {
            if (xmlState->hasTagName(apvts.state.getType()))
                apvts.replaceState(juce::ValueTree::fromXml(*xmlState));

            if (xmlState->hasAttribute("helperSourceUuid"))
            {
                originalStateUuid = xmlState->getStringAttribute("helperSourceUuid");
                apvts.state.setProperty("helperSourceUuid", originalStateUuid, nullptr);
            }
        }

        scheduleHelperSourceConnection();
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
    std::atomic<bool> connectionScheduled{false};
    std::atomic<bool> sourceConnected{false};
    mutable std::mutex processingMutex;
    bool usingFallbackSource = false;
    juce::String originalStateUuid;

    static void frontendEventCallback(enum obs_frontend_event event, void* private_data)
    {
        auto* self = static_cast<ObsOutputAudioProcessor*>(private_data);

        if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
        {
            self->connectionScheduled.store(false, std::memory_order_release);
            self->releaseHelperSource();
            return;
        }

        if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING)
        {
            self->connectionScheduled.store(false, std::memory_order_release);
            self->releaseHelperSource();
        }

        if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
        {
            if (!self->usingFallbackSource || !self->privateSource)
                return;

            auto fallBackUuid = juce::String(obs_source_get_uuid(self->privateSource));
            if (fallBackUuid.isEmpty() || fallBackUuid == self->originalStateUuid)
                return;

            obs_source_t* originalObsSource = self->findSourceByUuid(self->originalStateUuid.toStdString());
            if (!originalObsSource)
                return;

            obs_source_release(originalObsSource);

            self->apvts.state.setProperty("helperSourceUuid", self->originalStateUuid, nullptr);
            self->releaseHelperSource(true);
            self->pairWithHelperByUuid(self->originalStateUuid.toStdString());
        }
    }

    void scheduleHelperSourceConnection()
    {
        if (connectionScheduled.exchange(true, std::memory_order_acq_rel))
            return;

        if (sourceConnected.load(std::memory_order_acquire))
        {
            connectionScheduled.store(false, std::memory_order_release);
            return;
        }

        juce::Timer::callAfterDelay(
            2000,
            [this]()
            {
                juce::MessageManager::callAsync(
                    [this]()
                    {
                        if (sourceConnected.load(std::memory_order_acquire))
                        {
                            connectionScheduled.store(false, std::memory_order_release);
                            return;
                        }

                        std::unique_ptr<juce::XmlElement> xml(apvts.state.createXml());
                        juce::String uuidValue;
                        if (xml && xml->hasAttribute("helperSourceUuid"))
                            uuidValue = xml->getStringAttribute("helperSourceUuid");

                        if (uuidValue.isNotEmpty())
                        {
                            pairWithHelperByUuid(uuidValue.toStdString());
                            connectionScheduled.store(false, std::memory_order_release);
                            return;
                        }

                        createNewHelperSource();
                        connectionScheduled.store(false, std::memory_order_release);
                    }
                );
            }
        );
    }

    void releaseHelperSource(bool removeFromScene = false)
    {
        std::lock_guard<std::mutex> lock(processingMutex);

        if (!privateSource)
            return;

        if (removeFromScene)
            obs_source_remove(privateSource);
        obs_source_release(privateSource);
        privateSource = nullptr;
        sourceConnected.store(false, std::memory_order_release);
        usingFallbackSource = false;
    }

    obs_source_t* findSourceByUuid(const std::string& uuid)
    {
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
                    ctx->foundSource = obs_source_get_ref(source);
                    return false;
                }
                return true;
            },
            &context
        );

        return context.foundSource;
    }

    void pairWithHelperByUuid(const std::string& uuid)
    {
        obs_source_t* foundSource = findSourceByUuid(uuid);

        std::lock_guard<std::mutex> lock(processingMutex);

        if (privateSource)
        {
            obs_source_release(privateSource);
            privateSource = nullptr;
        }

        if (foundSource)
        {
            privateSource = foundSource;
            usingFallbackSource = false;
            sourceConnected.store(true, std::memory_order_release);
            return;
        }

        createFallbackSourceLocked();
    }

    void createNewHelperSource()
    {
        std::lock_guard<std::mutex> lock(processingMutex);
        createFallbackSourceLocked();
    }

    // Must be called with processingMutex held
    void createFallbackSourceLocked()
    {
        privateSource = obs_source_create("atkaudio_ph2helper", "Ph2Out", nullptr, nullptr);
        if (!privateSource)
            return;

        obs_source_set_audio_active(privateSource, true);
        obs_source_set_enabled(privateSource, true);
        usingFallbackSource = true;
        sourceConnected.store(true, std::memory_order_release);

        auto* currentScene = obs_frontend_get_current_scene();
        if (currentScene)
        {
            auto* sceneSource = obs_scene_from_source(currentScene);
            if (sceneSource)
                obs_scene_add(sceneSource, privateSource);
            obs_source_release(currentScene);
        }

        const char* sourceUuid = obs_source_get_uuid(privateSource);
        if (sourceUuid && sourceUuid[0] != '\0')
            apvts.state.setProperty("helperSourceUuid", juce::String(sourceUuid), nullptr);
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