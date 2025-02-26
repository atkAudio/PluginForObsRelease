#pragma once

#include "../LookAndFeel.h"
#include "AudioDeviceSelectorComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

using namespace juce;

class SettingsComponent : public Component
{
public:
    SettingsComponent(
        // AudioAppDemo& pluginHolder,
        AudioDeviceManager& deviceManagerToUse,
        int maxAudioInputChannels,
        int maxAudioOutputChannels
    )
        : // owner(pluginHolder),
        deviceSelector(
            deviceManagerToUse,
            0,
            maxAudioInputChannels,
            0,
            maxAudioOutputChannels,
            false,
            false,
            false,
            true
        )
        , shouldMuteLabel("Feedback Loop:", "Feedback Loop:")
        , shouldMuteButton("Mute audio input")
    {
        setOpaque(true);

        addAndMakeVisible(deviceSelector);
    }

    void paint(Graphics& g) override
    {
        g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        const ScopedValueSetter<bool> scope(isResizing, true);

        auto r = getLocalBounds();

        deviceSelector.setBounds(r);
    }

    void childBoundsChanged(Component* childComp) override
    {
        if (!isResizing && childComp == &deviceSelector)
            setToRecommendedSize();
    }

    void setToRecommendedSize()
    {
        const auto extraHeight = [&]
        {
            const auto itemHeight = deviceSelector.getItemHeight();
            const auto separatorHeight = (itemHeight >> 1);
            return itemHeight + separatorHeight;
        }();

        setSize(getWidth(), deviceSelector.getHeight() + extraHeight);
    }

private:
    //==============================================================================
    atk::AudioDeviceSelectorComponent deviceSelector;
    Label shouldMuteLabel;
    ToggleButton shouldMuteButton;
    bool isResizing = false;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsComponent)
};