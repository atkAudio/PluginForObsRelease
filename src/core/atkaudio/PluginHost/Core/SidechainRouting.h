#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace atk
{
// What feeds the rows of the PluginHost input routing matrix, in row order:
// OBS main channels, OBS sidechain channels, then device inputs.
struct SidechainFeedSources
{
    int numObsMainChannels = 0;
    int numObsSidechainChannels = 0;
    bool obsSidechainSelected = false;
    int numDeviceInputs = 0;
};

// True when the matrix routes a live source into any plugin sidechain column. The plugin
// has only a main and a sidechain input bus, so every column from firstSidechainColumn
// (the plugin's main input channel count) on is sidechain. The OBS sidechain rows only
// count while an OBS sidechain source is selected.
bool IsSidechainFed(
    const std::vector<std::vector<bool>>& inputMapping,
    const SidechainFeedSources& sources,
    int firstSidechainColumn
);

struct InnerLayoutChoice
{
    juce::AudioProcessor::BusesLayout layout;
    bool supported = false;
    // Channels the hosted plugin's sidechain bus takes when enabled; 0 when it has none.
    int numSidechainChannels = 0;
};

// Picks the hosted plugin's layout from the PluginHost layout (main bus, sidechain bus).
// The sidechain bus is enabled only while it is fed, because some plugins (e.g. Waves)
// key from an active sidechain bus even when it carries silence.
InnerLayoutChoice
ChooseInnerLayout(juce::AudioProcessor* inner, const juce::AudioProcessor::BusesLayout& hostLayout, bool sidechainFed);
} // namespace atk
