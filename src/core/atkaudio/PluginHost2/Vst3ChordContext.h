#pragma once

#include "InternalPlugins/ChordAnalyzer.h"
#include <juce_audio_processors/juce_audio_processors.h>

class Vst3ChordContext : public juce::AudioPluginExtensions::VST3Client::ProcessContextProvider
{
public:
    explicit Vst3ChordContext(std::shared_ptr<chord_analyzer::PublishedChord>);
    void beginBlock();
    void reset();
    void updateProcessContext(Steinberg::Vst::ProcessContext&) override;

private:
    std::shared_ptr<chord_analyzer::PublishedChord> published;
    std::atomic<uint32_t> snapshot{0};
};
