#pragma once

#include "../AudioModule.h"

#include <string>

namespace atk
{
class DeviceIo : public atkAudioModule
{
public:
    DeviceIo();
    ~DeviceIo();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate) override;

    void setMixInput(bool mixInput);

    /**
     * Set output delay in milliseconds (applied before sending to hardware).
     * Range: 0-10000 ms
     */
    void setOutputDelay(float delayMs);

    /**
     * Get current output delay in milliseconds
     */
    float getOutputDelay() const;

    void getState(std::string& s) override;
    void setState(std::string& s) override;

protected:
    // AudioModule interface - only need to provide the window component
    juce::Component* getWindowComponent() override;

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk