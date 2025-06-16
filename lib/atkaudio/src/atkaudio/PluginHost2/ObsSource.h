#pragma once

#include "../FifoBuffer2.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <obs-module.h>
#include <string>
#include <vector>

#define PROPERTY_NAME "source"
#define CHILD_NAME "SelectedSource"

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

static juce::StringArray GetObsAudioSources(obs_source_t* parentSource = nullptr)
{
    juce::StringArray sourceNames;

    obs_enum_sources(
        [](void* param, obs_source_t* src)
        {
            auto* names = static_cast<juce::StringArray*>(param);
            const char* name = obs_source_get_name(src);
            uint32_t caps = obs_source_get_output_flags(src);

            if ((caps & OBS_SOURCE_AUDIO) == 0)
                return true;

            if (name && juce::String(name).containsIgnoreCase("ph2out"))
                return true;

            if (name)
                names->add(juce::String(name));
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
        : apvts(*this, nullptr, "Parameters", {})
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

    void removeObsAudioCaptureCallback()
    {
        auto sourceName =
            apvts.state.getOrCreateChildWithName(CHILD_NAME, nullptr).getProperty(PROPERTY_NAME).toString();
        if (sourceName.isEmpty())
            return;

        obs_source_t* source = obs_get_source_by_name(sourceName.toRawUTF8());
        if (source)
        {
            obs_source_remove_audio_capture_callback(source, obs_capture_callback, this);
            obs_source_release(source);
        }
        if (currentObsSource)
        {
            obs_source_release(currentObsSource);
            currentObsSource = nullptr;
        }
    }

    void addObsAudioCaptureCallback()
    {
        removeObsAudioCaptureCallback();
        auto sourceName =
            apvts.state.getOrCreateChildWithName(CHILD_NAME, nullptr).getProperty(PROPERTY_NAME).toString();

        obs_source_t* source = obs_get_source_by_name(sourceName.toRawUTF8());
        if (source)
        {
            obs_source_add_audio_capture_callback(source, obs_capture_callback, this);
            obs_source_set_muted(source, true);
            currentObsSource = source;
        }
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        syncBuffer.read(
            buffer.getArrayOfWritePointers(),
            getMainBusNumInputChannels(),
            buffer.getNumSamples(),
            getSampleRate()
        );
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
        int numChannels = processor->getMainBusNumInputChannels();
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
    {
        auto sources = GetObsAudioSources();
        auto savedSource =
            processor.getApvts().state.getOrCreateChildWithName(CHILD_NAME, nullptr).getProperty(PROPERTY_NAME);
        addAndMakeVisible(listBox);

        auto selectedIdx = sources.indexOf(savedSource.toString(), false);

        setSize(300, 200);
        setResizable(true, true);
        setResizeLimits(200, 100, 300, 600);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);

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
                items.add(item);
            setModel(this);
            setOutlineThickness(1);
        }

        int getNumRows() override
        {
            return items.size();
        }

        void paintListBoxItem(int row, Graphics& g, int width, int height, bool rowIsSelected) override
        {
            auto& state = processor.getApvts().state;
            auto selectedSource =
                state.getOrCreateChildWithName(CHILD_NAME, nullptr).getProperty(PROPERTY_NAME).toString();
            if (isPositiveAndBelow(row, items.size()))
            {
                if (rowIsSelected)
                    g.fillAll(findColour(TextEditor::highlightColourId).withMultipliedAlpha(0.3f));

                auto item = items[row];
                bool enabled = (item == selectedSource);

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

                drawTextLayout(g, *this, item, {x + 5, 0, width - x - 5, height}, enabled);
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

            if (items.isEmpty())
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
        juce::Array<juce::String> items;

        void flipEnablement(const int row)
        {
            if (isPositiveAndBelow(row, items.size()))
            {
                auto sourceName = items[row];
                auto& state = processor.getApvts().state;
                auto currentSelected =
                    state.getOrCreateChildWithName(CHILD_NAME, nullptr).getProperty(PROPERTY_NAME).toString();

                if (sourceName.isNotEmpty())
                {
                    if (currentSelected == sourceName)
                    {
                        processor.removeObsAudioCaptureCallback();
                        state.getOrCreateChildWithName(CHILD_NAME, nullptr).removeProperty(PROPERTY_NAME, nullptr);
                    }
                    else
                    {
                        state.getOrCreateChildWithName(CHILD_NAME, nullptr)
                            .setProperty(PROPERTY_NAME, sourceName, nullptr);
                        processor.addObsAudioCaptureCallback();
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
};

// Implementation of createEditor
inline juce::AudioProcessorEditor* ObsSourceAudioProcessor::createEditor()
{
    return new ObsSourceAudioProcessorEditor(*this);
}