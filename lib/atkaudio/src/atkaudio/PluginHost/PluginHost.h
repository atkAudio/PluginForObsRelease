#pragma once

#include "../AudioModule.h"

#include <string>

namespace atk
{
class PluginHost : public atkAudioModule
{
public:
    PluginHost();
    ~PluginHost();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate);

    // WindowModule interface
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