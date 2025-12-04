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

    int getInnerPluginChannelCount() const;

protected:
    juce::Component* getWindowComponent() override;

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk