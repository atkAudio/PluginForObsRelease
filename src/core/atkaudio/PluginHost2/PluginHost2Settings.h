#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

struct PluginHost2Settings
{
    PluginHost2Settings();
    explicit PluginHost2Settings(const juce::File&);

    bool saveWindowState(juce::ResizableWindow&, bool flush = false);

    juce::InterProcessLock processLock{"atkAudioPluginHost2Lock"};
    std::unique_ptr<juce::PropertiesFile> properties;
};
