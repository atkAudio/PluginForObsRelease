#pragma once

#include "../AudioModule.h"

#include <string>
#include <vector>

namespace atk
{
/**
 * DeviceIo2 - Audio device I/O with routing matrix.
 *
 * Similar to DeviceIo but uses a routing matrix approach like PluginHost.
 * Instead of routing audio through a plugin:
 * - Input side: mixes audio from OBS and device inputs into internal buffer based on routing matrix
 * - Output side: outputs from internal buffer to device outputs based on routing matrix
 */
class DeviceIo2 : public atkAudioModule
{
public:
    DeviceIo2();
    ~DeviceIo2();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate) override;

    /**
     * Set output delay in milliseconds (applied before sending to hardware).
     * Range: 0-10000 ms
     */
    void setOutputDelay(float delayMs);

    /**
     * Get current output delay in milliseconds
     */
    float getOutputDelay() const;

    /**
     * Set channel mapping for input routing.
     * Controls which OBS + device input channels mix into the internal buffer.
     * @param mapping 2D array [sourceChannel][internalChannel] = enabled
     *                First rows are OBS channels, remaining rows are device input channels
     */
    void setInputChannelMapping(const std::vector<std::vector<bool>>& mapping);

    /**
     * Get current input channel mapping
     */
    std::vector<std::vector<bool>> getInputChannelMapping() const;

    /**
     * Set channel mapping for output routing.
     * Controls which internal buffer channels pass through to device outputs.
     * @param mapping 2D array [internalChannel][deviceOutputChannel] = enabled
     */
    void setOutputChannelMapping(const std::vector<std::vector<bool>>& mapping);

    /**
     * Get current output channel mapping
     */
    std::vector<std::vector<bool>> getOutputChannelMapping() const;

    void getState(std::string& s) override;
    void setState(std::string& s) override;

    // Public access to window component for embedding in other contexts (e.g., PluginHost2)
    juce::Component* getWindowComponent() override;

    // Get the settings component directly for embedding (without DocumentWindow wrapper)
    // Returns a newly created AudioServerSettingsComponent that the caller owns
    juce::Component* createEmbeddableSettingsComponent();

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk
