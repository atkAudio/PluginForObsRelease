#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <media-io/audio-io.h>
#include <obs-module.h>

static juce::AudioChannelSet getChannelSetForCount(int numChannels)
{
    if (numChannels <= 0 || numChannels == 2)
        return juce::AudioChannelSet::stereo();

    if (numChannels == 1)
        return juce::AudioChannelSet::mono();

    switch (numChannels)
    {
    case 3:
        return juce::AudioChannelSet::createLCR();
    case 4:
        return juce::AudioChannelSet::quadraphonic();
    case 5:
        return juce::AudioChannelSet::create5point0();
    case 6:
        return juce::AudioChannelSet::create5point1();
    case 7:
        return juce::AudioChannelSet::create7point0();
    case 8:
        return juce::AudioChannelSet::create7point1();
    default:
        return juce::AudioChannelSet::discreteChannels(numChannels);
    }
}

static juce::AudioChannelSet getDefaultObsAudioChannelSet()
{
    auto* obsAudio = obs_get_audio();
    if (obsAudio == nullptr)
        return juce::AudioChannelSet::stereo();

    auto numChannels = static_cast<int>(audio_output_get_channels(obsAudio));
    return getChannelSetForCount(numChannels);
}
