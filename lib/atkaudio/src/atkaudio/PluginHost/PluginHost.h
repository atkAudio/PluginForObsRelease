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
    void setVisible(bool visible) override;

    void setDockId(const std::string& id);
    bool isDockVisible() const;

    int getInnerPluginChannelCount() const;

    bool isMultiCoreEnabled() const;

    void setMultiCoreEnabled(bool enabled);

    float getCpuLoad() const;

    int getLatencyMs() const;

protected:
    juce::Component* getWindowComponent() override;

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk