#pragma once

#include "../../atkAudioModule.h"

#include <string>

namespace atk
{
class PluginHost2 : public atkAudioModule
{
public:
    PluginHost2();
    ~PluginHost2();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate) override;

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