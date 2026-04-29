#pragma once

#include <atkaudio/AtomicSharedPtr.h>

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace atk
{

struct ChannelMappingState
{
    std::vector<std::vector<bool>> inputMapping;
    std::vector<std::vector<bool>> outputMapping;
    std::atomic<bool> debugLogged{false};
};

class ChannelRoutingMatrix
{
public:
    ChannelRoutingMatrix();
    ~ChannelRoutingMatrix() = default;

    void applyInputRouting(
        float* const* obsBuffer,
        const juce::AudioBuffer<float>& deviceInputBuffer,
        juce::AudioBuffer<float>& targetBuffer,
        int numObsChannels,
        int numSamples,
        int numDeviceInputSubs
    );

    void applyOutputRouting(
        const juce::AudioBuffer<float>& sourceBuffer,
        float* const* obsBuffer,
        juce::AudioBuffer<float>& deviceOutputBuffer,
        int numObsChannels,
        int numSamples,
        int numDeviceOutputSubs
    );

    void setInputMapping(const std::vector<std::vector<bool>>& mapping);
    std::vector<std::vector<bool>> getInputMapping() const;

    void setOutputMapping(const std::vector<std::vector<bool>>& mapping);
    std::vector<std::vector<bool>> getOutputMapping() const;

    void initializeDefaultMapping(int numChannels);
    void resizeMappings(int numChannels);

private:
    // Atomic shared pointer to channel mapping state
    atk::AtomicSharedPtr<ChannelMappingState> mappingState;
};

} // namespace atk
