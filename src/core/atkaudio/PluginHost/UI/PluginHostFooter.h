#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class PluginHostFooter final
    : public juce::Component
    , private juce::Timer
{
public:
    PluginHostFooter(const juce::String& actionButtonText, bool showLink = false)
        : showLinkButton(showLink)
    {
        actionButton.setButtonText(actionButtonText);

        // MT button for enabling secondary job queue processing
        addAndMakeVisible(multiToggle);
        addAndMakeVisible(audioButton);
        addAndMakeVisible(midiButton);
        addAndMakeVisible(actionButton);

        multiToggle.setButtonText("MT");
        multiToggle.setTooltip("Multi-threading (extra buffer latency)");
        multiToggle.setClickingTogglesState(true);

        statsLabel.setFont(juce::FontOptions(10.0f));
        statsLabel.setJustificationType(juce::Justification::centredLeft);
        statsLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
        statsLabel.setBorderSize(juce::BorderSize<int>(0, 4, 0, 0));
        addAndMakeVisible(statsLabel);

        if (showLinkButton)
        {
            linkButton.setFont(juce::FontOptions(11.0f), false);
            addAndMakeVisible(linkButton);
        }

        startTimerHz(10);
    }

    ~PluginHostFooter() override
    {
        stopTimer();
    }

    void setMultiCoreCallbacks(std::function<bool()> getEnabledCallback, std::function<void(bool)> setEnabledCallback)
    {
        getMultiCoreEnabled = getEnabledCallback;

        if (getEnabledCallback)
            multiToggle.setToggleState(getEnabledCallback(), juce::dontSendNotification);

        if (setEnabledCallback)
            multiToggle.onClick = [this, setEnabledCallback] { setEnabledCallback(multiToggle.getToggleState()); };
    }

    void setStatsCallbacks(std::function<float()> getCpuLoadFn, std::function<int()> getLatencyMsFn)
    {
        getCpuLoad = getCpuLoadFn;
        getLatencyMs = getLatencyMsFn;
    }

    void timerCallback() override
    {
        if (getMultiCoreEnabled)
        {
            bool currentState = getMultiCoreEnabled();
            if (multiToggle.getToggleState() != currentState)
                multiToggle.setToggleState(currentState, juce::dontSendNotification);
        }

        float cpuLoad = getCpuLoad ? getCpuLoad() : 0.0f;
        int latencyMs = getLatencyMs ? getLatencyMs() : 0;

        statsLabel.setText(
            juce::String(latencyMs) + "ms " + juce::String(cpuLoad, 2).replace("0.", "."),
            juce::dontSendNotification
        );
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        const int statsHeight = 14;
        auto statsArea = bounds.removeFromBottom(statsHeight);

        statsLabel.setBounds(statsArea.removeFromLeft(60));

        juce::Grid grid;
        grid.autoFlow = juce::Grid::AutoFlow::column;
        grid.setGap(juce::Grid::Px{5});
        grid.autoRows = grid.autoColumns = juce::Grid::TrackInfo{juce::Grid::Fr{1}};

        grid.items = {
            juce::GridItem{multiToggle}.withSize(60.0f, (float)bounds.getHeight()),
            juce::GridItem{audioButton}
                .withSize((float)audioButton.getBestWidthForHeight(40), (float)bounds.getHeight()),
            juce::GridItem{midiButton}.withSize((float)midiButton.getBestWidthForHeight(40), (float)bounds.getHeight()),
            juce::GridItem{actionButton}
                .withSize((float)actionButton.getBestWidthForHeight(40), (float)bounds.getHeight())
        };

        if (showLinkButton)
            grid.items.add(juce::GridItem{linkButton});

        grid.performLayout(bounds);

        if (showLinkButton)
        {
            linkButton.changeWidthToFitText();
            linkButton.setTopRightPosition(getWidth(), 0);
        }
    }

    juce::ToggleButton multiToggle;
    juce::TextButton audioButton{"Audio..."};
    juce::TextButton midiButton{"MIDI..."};
    juce::TextButton actionButton;
    juce::Label statsLabel;
    juce::HyperlinkButton linkButton{"atkAudio", juce::URL("http://www.atkaudio.com")};

private:
    bool showLinkButton = false;
    std::function<bool()> getMultiCoreEnabled;
    std::function<float()> getCpuLoad;
    std::function<int()> getLatencyMs;
    juce::SharedResourcePointer<juce::TooltipWindow> tooltipWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHostFooter)
};
