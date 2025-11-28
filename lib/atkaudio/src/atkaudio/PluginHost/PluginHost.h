#pragma once

#include "../atkAudioModule.h"

#include <string>

namespace atk
{
class PluginHost : public atkAudioModule
{
public:
    PluginHost();
    ~PluginHost();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate) override;

    // WindowModule interface
    void getState(std::string& s) override;
    void setState(std::string& s) override;

    // Get the channel count of the loaded inner plugin
    int getInnerPluginChannelCount() const;

    // Check if sidechain input is enabled
    bool isSidechainEnabled() const;

    // Enable/disable sidechain input
    void setSidechainEnabled(bool enabled);

    // Check if multi-core processing is enabled
    bool isMultiCoreEnabled() const;

    // Enable/disable multi-core processing (threadpool vs synchronous)
    void setMultiCoreEnabled(bool enabled);

protected:
    // AudioModule interface - only need to provide the window component
    juce::Component* getWindowComponent() override;

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk