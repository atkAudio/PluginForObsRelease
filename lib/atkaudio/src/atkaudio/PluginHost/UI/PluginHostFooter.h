#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/**
 * Shared footer component for PluginHost UI.
 * Contains Audio/MIDI buttons and a customizable action button.
 */
class PluginHostFooter final : public juce::Component
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
        multiToggle.setTooltip("Enable multithreading using secondary job queue");
        multiToggle.setClickingTogglesState(true);

        if (showLinkButton)
        {
            linkButton.setFont(juce::Font(11.0f), false);
            addAndMakeVisible(linkButton);
        }
    }

    void setMultiCoreCallbacks(std::function<bool()> getEnabledCallback, std::function<void(bool)> setEnabledCallback)
    {
        // Set initial state
        if (getEnabledCallback)
            multiToggle.setToggleState(getEnabledCallback(), juce::dontSendNotification);

        // Set click handler
        if (setEnabledCallback)
            multiToggle.onClick = [this, setEnabledCallback] { setEnabledCallback(multiToggle.getToggleState()); };
    }

    void resized() override
    {
        juce::Grid grid;
        grid.autoFlow = juce::Grid::AutoFlow::column;
        grid.setGap(juce::Grid::Px{5});
        grid.autoRows = grid.autoColumns = juce::Grid::TrackInfo{juce::Grid::Fr{1}};

        grid.items = {
            juce::GridItem{multiToggle}.withSize(60.0f, (float)getHeight()),
            juce::GridItem{audioButton}.withSize((float)audioButton.getBestWidthForHeight(40), (float)getHeight()),
            juce::GridItem{midiButton}.withSize((float)midiButton.getBestWidthForHeight(40), (float)getHeight()),
            juce::GridItem{actionButton}.withSize((float)actionButton.getBestWidthForHeight(40), (float)getHeight())
        };

        if (showLinkButton)
            grid.items.add(juce::GridItem{linkButton});

        grid.performLayout(getLocalBounds());

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
    juce::HyperlinkButton linkButton{"atkAudio", juce::URL("http://www.atkaudio.com")};

private:
    bool showLinkButton = false;
    juce::SharedResourcePointer<juce::TooltipWindow> tooltipWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHostFooter)
};
