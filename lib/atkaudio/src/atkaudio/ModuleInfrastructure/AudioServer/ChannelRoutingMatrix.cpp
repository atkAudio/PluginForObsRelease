#include "ChannelRoutingMatrix.h"

namespace atk
{

ChannelRoutingMatrix::ChannelRoutingMatrix()
{
    // Initialize with default stereo pass-through
    initializeDefaultMapping(2);
}

void ChannelRoutingMatrix::initializeDefaultMapping(int numChannels)
{
    auto state = std::make_shared<ChannelMappingState>();
    state->inputMapping.resize(numChannels);
    state->outputMapping.resize(numChannels);

    for (int i = 0; i < numChannels; ++i)
    {
        state->inputMapping[i].resize(numChannels, false);
        state->inputMapping[i][i] = true; // Diagonal pass-through

        state->outputMapping[i].resize(numChannels, false);
        state->outputMapping[i][i] = true; // Diagonal pass-through
    }

    mappingState.store(std::move(state), std::memory_order_release);
}

void ChannelRoutingMatrix::resizeMappings(int numChannels)
{
    auto oldState = mappingState.load(std::memory_order_acquire);
    if (!oldState
        || ((int)oldState->inputMapping.size() == numChannels && (int)oldState->outputMapping.size() == numChannels))
    {
        // Already correct size
        return;
    }

    auto newState = std::make_shared<ChannelMappingState>();

    // Resize input mapping
    newState->inputMapping.resize(numChannels);
    for (int i = 0; i < numChannels; ++i)
    {
        newState->inputMapping[i].resize(numChannels, false);
        // Preserve old mappings where possible
        if (oldState && i < (int)oldState->inputMapping.size())
        {
            for (int j = 0; j < numChannels && j < (int)oldState->inputMapping[i].size(); ++j)
                newState->inputMapping[i][j] = oldState->inputMapping[i][j];
        }
        else
        {
            // New channel - default to diagonal
            newState->inputMapping[i][i] = true;
        }
    }

    // Resize output mapping
    newState->outputMapping.resize(numChannels);
    for (int i = 0; i < numChannels; ++i)
    {
        newState->outputMapping[i].resize(numChannels, false);
        // Preserve old mappings where possible
        if (oldState && i < (int)oldState->outputMapping.size())
        {
            for (int j = 0; j < numChannels && j < (int)oldState->outputMapping[i].size(); ++j)
                newState->outputMapping[i][j] = oldState->outputMapping[i][j];
        }
        else
        {
            // New channel - default to diagonal
            newState->outputMapping[i][i] = true;
        }
    }

    mappingState.store(std::move(newState), std::memory_order_release);
}

void ChannelRoutingMatrix::applyInputRouting(
    float* const* obsBuffer,
    const juce::AudioBuffer<float>& deviceInputBuffer,
    juce::AudioBuffer<float>& targetBuffer,
    int numObsChannels,
    int numSamples,
    int numDeviceInputSubs
)
{
    auto state = mappingState.load(std::memory_order_acquire);
    if (!state || state->inputMapping.empty())
    {
        // No routing matrix - direct pass-through
        for (int ch = 0; ch < numObsChannels && ch < targetBuffer.getNumChannels(); ++ch)
            targetBuffer.copyFrom(ch, 0, obsBuffer[ch], numSamples);
        return;
    }

    int matrixRows = (int)state->inputMapping.size();
    int numTargetChannels = targetBuffer.getNumChannels();
    int deviceStartRow = matrixRows - numDeviceInputSubs;
    if (deviceStartRow < 0)
        deviceStartRow = numObsChannels;

    // Clear target buffer before routing
    targetBuffer.clear();

    // Route OBS channels to target buffer
    // Matrix logic: If Row (OBS channel) maps to Column (target channel), add/mix it
    for (int obsChannel = 0; obsChannel < numObsChannels && obsChannel < deviceStartRow; ++obsChannel)
    {
        if (obsChannel < matrixRows)
        {
            for (int targetChannel = 0;
                 targetChannel < numTargetChannels && targetChannel < (int)state->inputMapping[obsChannel].size();
                 ++targetChannel)
            {
                if (state->inputMapping[obsChannel][targetChannel])
                    targetBuffer.addFrom(targetChannel, 0, obsBuffer[obsChannel], numSamples);
            }
        }
    }

    // Route device input channels to target buffer (mixing)
    // Matrix logic: If Row (device channel) maps to Column (target channel), add/mix it
    for (int subIdx = 0; subIdx < numDeviceInputSubs; ++subIdx)
    {
        int matrixRow = deviceStartRow + subIdx;
        if (matrixRow < matrixRows)
        {
            for (int targetChannel = 0;
                 targetChannel < numTargetChannels && targetChannel < (int)state->inputMapping[matrixRow].size();
                 ++targetChannel)
            {
                if (state->inputMapping[matrixRow][targetChannel])
                    targetBuffer.addFrom(targetChannel, 0, deviceInputBuffer, subIdx, 0, numSamples);
            }
        }
    }
}

void ChannelRoutingMatrix::applyOutputRouting(
    const juce::AudioBuffer<float>& sourceBuffer,
    float* const* obsBuffer,
    juce::AudioBuffer<float>& deviceOutputBuffer,
    int numObsChannels,
    int numSamples,
    int numDeviceOutputSubs
)
{
    auto state = mappingState.load(std::memory_order_acquire);
    deviceOutputBuffer.clear();

    if (!state || state->outputMapping.empty())
    {
        // No routing matrix - direct pass-through
        for (int ch = 0; ch < numObsChannels && ch < sourceBuffer.getNumChannels(); ++ch)
            std::copy(sourceBuffer.getReadPointer(ch), sourceBuffer.getReadPointer(ch) + numSamples, obsBuffer[ch]);
        return;
    }

    int matrixRows = (int)state->outputMapping.size();
    int numSourceChannels = sourceBuffer.getNumChannels();
    int deviceStartRow = matrixRows - numDeviceOutputSubs;
    if (deviceStartRow < 0)
        deviceStartRow = numObsChannels;

    // Route source buffer to device outputs
    for (int subIdx = 0; subIdx < numDeviceOutputSubs; ++subIdx)
    {
        int matrixRow = deviceStartRow + subIdx;
        if (matrixRow < matrixRows)
        {
            for (int sourceChannel = 0;
                 sourceChannel < numSourceChannels && sourceChannel < (int)state->outputMapping[matrixRow].size();
                 ++sourceChannel)
            {
                if (state->outputMapping[matrixRow][sourceChannel])
                    deviceOutputBuffer.addFrom(subIdx, 0, sourceBuffer, sourceChannel, 0, numSamples);
            }
        }
    }

    // Route source buffer to OBS outputs
    // Matrix logic: If Row (OBS channel) maps to Column (source channel), add/mix it
    // Note: Output matrix rows are destinations (OBS), columns are sources (client/plugin)
    for (int obsChannel = 0; obsChannel < numObsChannels && obsChannel < deviceStartRow; ++obsChannel)
    {
        bool hasRouting = false;
        if (obsChannel < matrixRows)
        {
            // Clear this output channel first
            std::memset(obsBuffer[obsChannel], 0, numSamples * sizeof(float));

            // Mix all source channels that map to this OBS output channel
            for (int sourceChannel = 0;
                 sourceChannel < numSourceChannels && sourceChannel < (int)state->outputMapping[obsChannel].size();
                 ++sourceChannel)
            {
                if (state->outputMapping[obsChannel][sourceChannel])
                {
                    hasRouting = true;
                    // Add/mix source channel into OBS output channel
                    const float* src = sourceBuffer.getReadPointer(sourceChannel);
                    float* dst = obsBuffer[obsChannel];
                    for (int i = 0; i < numSamples; ++i)
                        dst[i] += src[i];
                }
            }
        }
        else
        {
            // No valid matrix row for this channel, zero it
            std::memset(obsBuffer[obsChannel], 0, numSamples * sizeof(float));
        }
    }
}

void ChannelRoutingMatrix::setInputMapping(const std::vector<std::vector<bool>>& mapping)
{
    auto oldState = mappingState.load(std::memory_order_acquire);
    auto newState = std::make_shared<ChannelMappingState>();

    // Copy old output mapping, update input mapping
    newState->inputMapping = mapping;
    if (oldState)
        newState->outputMapping = oldState->outputMapping;

    mappingState.store(std::move(newState), std::memory_order_release);
}

std::vector<std::vector<bool>> ChannelRoutingMatrix::getInputMapping() const
{
    auto state = mappingState.load(std::memory_order_acquire);
    return state ? state->inputMapping : std::vector<std::vector<bool>>();
}

void ChannelRoutingMatrix::setOutputMapping(const std::vector<std::vector<bool>>& mapping)
{
    auto oldState = mappingState.load(std::memory_order_acquire);
    auto newState = std::make_shared<ChannelMappingState>();

    // Copy old input mapping, update output mapping
    if (oldState)
        newState->inputMapping = oldState->inputMapping;
    newState->outputMapping = mapping;

    mappingState.store(std::move(newState), std::memory_order_release);
}

std::vector<std::vector<bool>> ChannelRoutingMatrix::getOutputMapping() const
{
    auto state = mappingState.load(std::memory_order_acquire);
    return state ? state->outputMapping : std::vector<std::vector<bool>>();
}

} // namespace atk
