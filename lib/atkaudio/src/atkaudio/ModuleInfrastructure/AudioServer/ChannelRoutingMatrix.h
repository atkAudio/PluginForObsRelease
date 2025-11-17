#pragma once

#include <atkaudio/AtomicSharedPtr.h>

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace atk
{

/**
 * Channel mapping state for OBS channel routing matrix
 * Handles routing between OBS channels, device subscriptions, and target channels
 */
struct ChannelMappingState
{
    std::vector<std::vector<bool>> inputMapping;  // [sourceChannel][targetChannel]
    std::vector<std::vector<bool>> outputMapping; // [sourceChannel][targetChannel]

    // Debug flag - set to false initially, set to true after first log
    std::atomic<bool> debugLogged{false};
};

/**
 * ChannelRoutingMatrix - Reusable routing matrix processor for audio modules
 *
 * Handles routing audio between:
 * - OBS channels (first N rows/columns of matrix)
 * - Device input/output subscriptions (remaining rows/columns)
 * - Target processing buffer
 *
 * Thread-safe with atomic state management for realtime audio processing.
 */
class ChannelRoutingMatrix
{
public:
    ChannelRoutingMatrix();
    ~ChannelRoutingMatrix() = default;

    /**
     * Apply input routing matrix: (OBS channels + device inputs) → target buffer
     *
     * @param obsBuffer Planar OBS audio buffer (array of channel pointers)
     * @param deviceInputBuffer Device input subscriptions (one channel per subscription)
     * @param targetBuffer Output buffer to write routed audio
     * @param numObsChannels Number of OBS channels in obsBuffer
     * @param numSamples Number of samples to process
     * @param numDeviceInputSubs Number of device input subscriptions
     *
     * Matrix format: inputMapping[sourceChannel][targetChannel]
     * - Rows 0 to numObsChannels-1: OBS channels
     * - Rows numObsChannels+: Device input subscriptions
     */
    void applyInputRouting(
        float* const* obsBuffer,
        const juce::AudioBuffer<float>& deviceInputBuffer,
        juce::AudioBuffer<float>& targetBuffer,
        int numObsChannels,
        int numSamples,
        int numDeviceInputSubs
    );

    /**
     * Apply output routing matrix: source buffer → (OBS channels + device outputs)
     *
     * @param sourceBuffer Input buffer with audio to route
     * @param obsBuffer Planar OBS audio buffer (array of channel pointers) to write to
     * @param deviceOutputBuffer Device output subscriptions buffer (one channel per subscription)
     * @param numObsChannels Number of OBS channels in obsBuffer
     * @param numSamples Number of samples to process
     * @param numDeviceOutputSubs Number of device output subscriptions
     *
     * Matrix format: outputMapping[sourceChannel][destChannel]
     * - Columns 0 to numObsChannels-1: OBS channels
     * - Columns numObsChannels+: Device output subscriptions
     */
    void applyOutputRouting(
        const juce::AudioBuffer<float>& sourceBuffer,
        float* const* obsBuffer,
        juce::AudioBuffer<float>& deviceOutputBuffer,
        int numObsChannels,
        int numSamples,
        int numDeviceOutputSubs
    );

    /**
     * Set input channel mapping
     * @param mapping 2D array [sourceChannel][targetChannel] = enabled
     */
    void setInputMapping(const std::vector<std::vector<bool>>& mapping);

    /**
     * Get current input channel mapping
     */
    std::vector<std::vector<bool>> getInputMapping() const;

    /**
     * Set output channel mapping
     * @param mapping 2D array [sourceChannel][destChannel] = enabled
     */
    void setOutputMapping(const std::vector<std::vector<bool>>& mapping);

    /**
     * Get current output channel mapping
     */
    std::vector<std::vector<bool>> getOutputMapping() const;

    /**
     * Initialize with default diagonal pass-through mapping
     * @param numChannels Number of channels for default diagonal routing
     */
    void initializeDefaultMapping(int numChannels);

    /**
     * Resize mapping matrices to accommodate channel count changes
     * Preserves existing mappings where possible, adds diagonal routing for new channels
     * @param numChannels New channel count
     */
    void resizeMappings(int numChannels);

private:
    // Atomic shared pointer to channel mapping state
    atk::AtomicSharedPtr<ChannelMappingState> mappingState;
};

} // namespace atk
