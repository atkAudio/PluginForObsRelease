#pragma once

#include "../atkAudioModule.h"

#include <string>

namespace atk
{
class DeviceIo : public atkAudioModule
{
public:
    DeviceIo();
    ~DeviceIo();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate) override;

    // Bypass processing when filter should be inactive (e.g., not in scene)
    void setBypass(bool shouldBypass);
    bool isBypassed() const;

    // Set the fade time for bypass transitions (in seconds)
    void setFadeTime(double seconds);

    void setMixInput(bool mixInput);
    void setOutputDelay(float delayMs);
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