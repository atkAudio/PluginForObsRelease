#include "SidechainRouting.h"

#include <algorithm>

namespace atk
{
static bool RowFeedsColumnsFrom(const std::vector<bool>& row, int firstColumn)
{
    int rowSize = (int)row.size();
    for (int column = firstColumn; column < rowSize; ++column)
        if (row[column])
            return true;
    return false;
}

bool IsSidechainFed(
    const std::vector<std::vector<bool>>& inputMapping,
    const SidechainFeedSources& sources,
    int firstSidechainColumn
)
{
    // Same row layout as ChannelRoutingMatrix::applyInputRouting:
    // device input rows are the last numDeviceInputs rows, OBS rows come before them.
    int numRows = (int)inputMapping.size();
    int deviceStartRow = std::max(0, numRows - sources.numDeviceInputs);

    int numLiveObsRows = sources.numObsMainChannels;
    if (sources.obsSidechainSelected)
        numLiveObsRows += sources.numObsSidechainChannels;

    for (int row = 0; row < numLiveObsRows && row < deviceStartRow; ++row)
        if (RowFeedsColumnsFrom(inputMapping[row], firstSidechainColumn))
            return true;

    for (int row = deviceStartRow; row < numRows; ++row)
        if (RowFeedsColumnsFrom(inputMapping[row], firstSidechainColumn))
            return true;

    return false;
}

InnerLayoutChoice
ChooseInnerLayout(juce::AudioProcessor* inner, const juce::AudioProcessor::BusesLayout& hostLayout, bool sidechainFed)
{
    InnerLayoutChoice choice;

    // Same fallback order PluginHost has always used: the host layout as is, then the
    // sidechain as stereo, then mono, then without the sidechain bus at all.
    juce::Array<juce::AudioProcessor::BusesLayout> candidates;
    candidates.add(hostLayout);
    if (hostLayout.inputBuses.size() > 1)
    {
        auto stereoSidechain = hostLayout;
        stereoSidechain.inputBuses.getReference(1) = juce::AudioChannelSet::stereo();
        candidates.add(stereoSidechain);

        auto monoSidechain = hostLayout;
        monoSidechain.inputBuses.getReference(1) = juce::AudioChannelSet::mono();
        candidates.add(monoSidechain);

        auto noSidechain = hostLayout;
        noSidechain.inputBuses.removeLast();
        candidates.add(noSidechain);
    }

    for (int i = 0; i < candidates.size(); ++i)
    {
        if (inner->checkBusesLayoutSupported(candidates.getReference(i)))
        {
            choice.layout = candidates.getReference(i);
            choice.supported = true;
            break;
        }
    }

    if (!choice.supported || choice.layout.inputBuses.size() < 2)
        return choice;

    choice.numSidechainChannels = choice.layout.inputBuses.getReference(1).size();

    if (!sidechainFed)
    {
        auto disabledSidechain = choice.layout;
        disabledSidechain.inputBuses.getReference(1) = juce::AudioChannelSet::disabled();
        if (inner->checkBusesLayoutSupported(disabledSidechain))
            choice.layout = disabledSidechain;
    }

    return choice;
}
} // namespace atk
