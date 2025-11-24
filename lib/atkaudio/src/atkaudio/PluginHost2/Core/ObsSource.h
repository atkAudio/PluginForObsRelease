#pragma once

#include "../../FifoBuffer2.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <obs-module.h>
#include <string>
#include <vector>

#define PROPERTY_NAME "source"
#define PROPERTY_UUID "sourceUuid"
#define CHILD_NAME "SelectedSource"
#define FOLLOW_VOLUME_PROPERTY "followVolume"
#define FOLLOW_VOLUME_CHILD "FollowVolumeSettings"
#define FOLLOW_MUTE_PROPERTY "followMute"
#define FOLLOW_MUTE_CHILD "FollowMuteSettings"

static void drawTextLayout(
    juce::Graphics& g,
    juce::Component& owner,
    juce::StringRef text,
    const juce::Rectangle<int>& textBounds,
    bool enabled
)
{
    const auto textColour =
        owner.findColour(juce::ListBox::textColourId, true).withMultipliedAlpha(enabled ? 1.0f : 0.6f);

    juce::AttributedString attributedString{text};
    attributedString.setColour(textColour);
    attributedString.setFont(owner.withDefaultMetrics(juce::FontOptions{(float)textBounds.getHeight() * 0.6f}));
    attributedString.setJustification(juce::Justification::centredLeft);
    attributedString.setWordWrap(juce::AttributedString::WordWrap::none);

    juce::TextLayout textLayout;
    textLayout.createLayout(attributedString, (float)textBounds.getWidth(), (float)textBounds.getHeight());
    textLayout.draw(g, textBounds.toFloat());
}

static std::vector<std::string> GetObsAudioSources(obs_source_t* parentSource = nullptr)
{
    std::vector<std::string> sourceNames;

    obs_enum_sources(
        [](void* param, obs_source_t* src)
        {
            auto* names = static_cast<std::vector<std::string>*>(param);
            const char* name = obs_source_get_name(src);
            uint32_t caps = obs_source_get_output_flags(src);

            if ((caps & OBS_SOURCE_AUDIO) == 0)
                return true;

            if (name && std::string(name).find("ph2out") != std::string::npos)
                return true;

            if (name)
                names->push_back(std::string(name));
            return true;
        },
        &sourceNames
    );

    return sourceNames;
}

class ObsSourceAudioProcessor : public juce::AudioProcessor
{
public:
    ObsSourceAudioProcessor()
        : juce::AudioProcessor(
              juce::AudioProcessor::BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)
          )
        , apvts(*this, nullptr, "Parameters", {})
    {
    }

    ~ObsSourceAudioProcessor() override
    {
        removeObsAudioCaptureCallback();
    }

    const juce::String getName() const override
    {
        return "OBS Source";
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
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

        addObsAudioCaptureCallback();
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

    obs_source_t* getCurrentObsSource() const
    {
        return currentObsSource;
    }

    void removeObsAudioCaptureCallback()
    {
        // Release the current source if we have one
        if (currentObsSource)
        {
            obs_source_remove_audio_capture_callback(currentObsSource, obs_capture_callback, this);
            obs_source_release(currentObsSource);
            currentObsSource = nullptr;
        }
    }

    void addObsAudioCaptureCallback()
    {
        removeObsAudioCaptureCallback();

        // Get UUID from state
        auto sourceUuidStr = apvts.state.getOrCreateChildWithName(CHILD_NAME, nullptr)
                                 .getProperty(PROPERTY_UUID)
                                 .toString()
                                 .toStdString();

        if (sourceUuidStr.empty())
            return;

        // Find source by UUID
        struct FindContext
        {
            const std::string* targetUuid;
            obs_source_t* foundSource;
        } context{&sourceUuidStr, nullptr};

        obs_enum_sources(
            [](void* param, obs_source_t* source) -> bool
            {
                auto* ctx = static_cast<FindContext*>(param);
                const char* sourceUuid = obs_source_get_uuid(source);

                if (sourceUuid && *ctx->targetUuid == sourceUuid)
                {
                    ctx->foundSource = obs_source_get_ref(source);
                    return false; // stop enumeration
                }
                return true; // continue enumeration
            },
            &context
        );

        obs_source_t* source = context.foundSource;
        if (source)
        {
            obs_source_add_audio_capture_callback(source, obs_capture_callback, this);

            auto followVolume = apvts.state.getOrCreateChildWithName(FOLLOW_VOLUME_CHILD, nullptr)
                                    .getProperty(FOLLOW_VOLUME_PROPERTY, false);
            if (!followVolume)
                obs_source_set_muted(source, true);

            currentObsSource = source;
        }
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        syncBuffer.read(
            buffer.getArrayOfWritePointers(),
            getMainBusNumOutputChannels(),
            buffer.getNumSamples(),
            getSampleRate()
        );

        auto followVolume = apvts.state.getOrCreateChildWithName(FOLLOW_VOLUME_CHILD, nullptr)
                                .getProperty(FOLLOW_VOLUME_PROPERTY, false);

        if (followVolume && currentObsSource)
        {
            float obsVolume = obs_source_get_volume(currentObsSource);
            if (obsVolume != 1.0f)
                buffer.applyGain(obsVolume);
        }

        auto followMute =
            apvts.state.getOrCreateChildWithName(FOLLOW_MUTE_CHILD, nullptr).getProperty(FOLLOW_MUTE_PROPERTY, false);

        if (followMute && currentObsSource)
        {
            bool obsMuted = obs_source_muted(currentObsSource);
            if (obsMuted)
                buffer.clear();
        }
    }

private:
    static void obs_capture_callback(void* param, obs_source_t* source, const struct audio_data* audio_data, bool muted)
    {
        auto* processor = static_cast<ObsSourceAudioProcessor*>(param);
        if (!processor)
            return;

        if (source != processor->currentObsSource)
            return;

        auto& fifo = processor->syncBuffer;
        auto frames = (int)audio_data->frames;

        int numChannelsObs = audio_output_get_channels(obs_get_audio());
        int numChannels = processor->getMainBusNumOutputChannels();
        numChannels = jmin(numChannels, numChannelsObs);

        fifo.write(
            reinterpret_cast<const float* const*>(audio_data->data),
            numChannels,
            frames,
            audio_output_get_sample_rate(obs_get_audio())
        );
    }

private:
    SyncBuffer syncBuffer;
    obs_source_t* currentObsSource = nullptr;
    juce::AudioProcessorValueTreeState apvts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ObsSourceAudioProcessor)
};

class ObsSourceAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    ObsSourceAudioProcessorEditor(ObsSourceAudioProcessor& p)
        : juce::AudioProcessorEditor(&p)
        , processor(p)
        , listBox(p)
        , followVolumeToggle("Follow Source Volume")
        , followMuteToggle("Follow Source Mute")
    {
        auto sources = GetObsAudioSources();
        auto savedSourceRaw = processor.getApvts()
                                  .state.getOrCreateChildWithName(CHILD_NAME, nullptr)
                                  .getProperty(PROPERTY_NAME)
                                  .toString()
                                  .toRawUTF8();

        followVolumeToggle.setButtonText("Follow Source Volume");
        auto followVolumeState = processor.getApvts()
                                     .state.getOrCreateChildWithName(FOLLOW_VOLUME_CHILD, nullptr)
                                     .getProperty(FOLLOW_VOLUME_PROPERTY, false);
        followVolumeToggle.setToggleState(followVolumeState, juce::dontSendNotification);

        followVolumeToggle.onClick = [this]()
        {
            auto& state = processor.getApvts().state;
            bool newState = followVolumeToggle.getToggleState();
            state.getOrCreateChildWithName(FOLLOW_VOLUME_CHILD, nullptr)
                .setProperty(FOLLOW_VOLUME_PROPERTY, newState, nullptr);

            obs_source_t* currentSource = processor.getCurrentObsSource();
            if (currentSource)
            {
                if (newState)
                    obs_source_set_muted(currentSource, false);
                else
                    obs_source_set_muted(currentSource, true);
            }
        };

        // Setup follow mute toggle checkbox
        followMuteToggle.setButtonText("Follow Source Mute");
        auto followMuteState = processor.getApvts()
                                   .state.getOrCreateChildWithName(FOLLOW_MUTE_CHILD, nullptr)
                                   .getProperty(FOLLOW_MUTE_PROPERTY, false);
        followMuteToggle.setToggleState(followMuteState, juce::dontSendNotification);

        followMuteToggle.onClick = [this]()
        {
            auto& state = processor.getApvts().state;
            bool newState = followMuteToggle.getToggleState();
            state.getOrCreateChildWithName(FOLLOW_MUTE_CHILD, nullptr)
                .setProperty(FOLLOW_MUTE_PROPERTY, newState, nullptr);
        };

        addAndMakeVisible(followVolumeToggle);
        addAndMakeVisible(followMuteToggle);
        addAndMakeVisible(listBox);

        auto selectedIdx = -1;
        for (size_t i = 0; i < sources.size(); ++i)
        {
            if (sources[i] == savedSourceRaw)
            {
                selectedIdx = static_cast<int>(i);
                break;
            }
        }

        setSize(300, 200);
        setResizable(true, true);
        setResizeLimits(200, 100, 300, 600);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);

        auto checkboxHeight = 24;
        followVolumeToggle.setBounds(area.removeFromTop(checkboxHeight));

        area.removeFromTop(4);

        followMuteToggle.setBounds(area.removeFromTop(checkboxHeight));

        area.removeFromTop(8);

        listBox.setBounds(area);
    }

private:
    //==============================================================================
    class MidiInputSelectorComponentListBox final
        : public ListBox
        , private ListBoxModel
    {
    public:
        MidiInputSelectorComponentListBox(ObsSourceAudioProcessor& p)
            : ListBox({}, nullptr)
            , processor(p)
        {
            for (const auto& item : GetObsAudioSources())
                items.push_back(item);
            setModel(this);
            setOutlineThickness(1);
        }

        int getNumRows() override
        {
            return static_cast<int>(items.size());
        }

        void paintListBoxItem(int row, Graphics& g, int width, int height, bool rowIsSelected) override
        {
            auto& state = processor.getApvts().state;
            auto selectedSourceUuid =
                state.getOrCreateChildWithName(CHILD_NAME, nullptr).getProperty(PROPERTY_UUID).toString().toStdString();

            if (isPositiveAndBelow(row, static_cast<int>(items.size())))
            {
                if (rowIsSelected)
                    g.fillAll(findColour(TextEditor::highlightColourId).withMultipliedAlpha(0.3f));

                auto itemNameRaw = items[row];

                // Check if this source is enabled by comparing UUIDs
                bool enabled = false;
                if (!selectedSourceUuid.empty())
                {
                    obs_source_t* source = obs_get_source_by_name(itemNameRaw.c_str());
                    if (source)
                    {
                        const char* sourceUuid = obs_source_get_uuid(source);
                        if (sourceUuid && selectedSourceUuid == sourceUuid)
                            enabled = true;
                        obs_source_release(source);
                    }
                }

                auto x = getTickX();
                auto tickW = (float)height * 0.75f;

                getLookAndFeel().drawTickBox(
                    g,
                    *this,
                    (float)x - tickW,
                    ((float)height - tickW) * 0.5f,
                    tickW,
                    tickW,
                    enabled,
                    true,
                    true,
                    false
                );

                juce::String displayText = juce::String::fromUTF8(itemNameRaw.c_str());
                drawTextLayout(g, *this, displayText, {x + 5, 0, width - x - 5, height}, enabled);
            }
        }

        void listBoxItemClicked(int row, const MouseEvent& e) override
        {
            selectRow(row);

            if (e.x < getTickX())
                flipEnablement(row);
        }

        void listBoxItemDoubleClicked(int row, const MouseEvent&) override
        {
            flipEnablement(row);
        }

        void returnKeyPressed(int row) override
        {
            flipEnablement(row);
        }

        void paint(Graphics& g) override
        {
            ListBox::paint(g);

            if (items.empty())
            {
                g.setColour(Colours::grey);
                g.setFont(0.5f * (float)getRowHeight());
                g.drawText("No OBS Sources", 0, 0, getWidth(), getHeight() / 2, Justification::centred, true);
            }
        }

        int getBestHeight(int preferredHeight)
        {
            auto extra = getOutlineThickness() * 2;

            return jmax(getRowHeight() * 2 + extra, jmin(getRowHeight() * getNumRows() + extra, preferredHeight));
        }

    private:
        ObsSourceAudioProcessor& processor;
        std::vector<std::string> items;

        void flipEnablement(const int row)
        {
            if (isPositiveAndBelow(row, static_cast<int>(items.size())))
            {
                auto sourceNameRaw = items[row];
                auto& state = processor.getApvts().state;
                auto currentSelectedUuid = state.getOrCreateChildWithName(CHILD_NAME, nullptr)
                                               .getProperty(PROPERTY_UUID)
                                               .toString()
                                               .toStdString();

                if (!sourceNameRaw.empty())
                {
                    // Find the source by name to get its UUID
                    obs_source_t* source = obs_get_source_by_name(sourceNameRaw.c_str());
                    if (source)
                    {
                        const char* sourceUuid = obs_source_get_uuid(source);
                        std::string sourceUuidStr = sourceUuid ? sourceUuid : "";
                        obs_source_release(source);

                        if (currentSelectedUuid == sourceUuidStr)
                        {
                            // Deselect
                            processor.removeObsAudioCaptureCallback();
                            state.getOrCreateChildWithName(CHILD_NAME, nullptr).removeProperty(PROPERTY_UUID, nullptr);
                            state.getOrCreateChildWithName(CHILD_NAME, nullptr).removeProperty(PROPERTY_NAME, nullptr);
                        }
                        else
                        {
                            // Select - save both UUID (for tracking) and name (for display)
                            state.getOrCreateChildWithName(CHILD_NAME, nullptr)
                                .setProperty(PROPERTY_UUID, juce::String(sourceUuidStr), nullptr);
                            state.getOrCreateChildWithName(CHILD_NAME, nullptr)
                                .setProperty(PROPERTY_NAME, juce::String(sourceNameRaw), nullptr);
                            processor.addObsAudioCaptureCallback();
                        }
                    }
                }
            }
            this->repaint();
        }

        int getTickX() const
        {
            return getRowHeight();
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiInputSelectorComponentListBox)
    };

    ObsSourceAudioProcessor& processor;
    MidiInputSelectorComponentListBox listBox;
    juce::ToggleButton followVolumeToggle;
    juce::ToggleButton followMuteToggle;
};

inline juce::AudioProcessorEditor* ObsSourceAudioProcessor::createEditor()
{
    return new ObsSourceAudioProcessorEditor(*this);
}