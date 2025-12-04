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