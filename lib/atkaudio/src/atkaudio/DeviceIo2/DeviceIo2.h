#pragma once

#include "../atkAudioModule.h"

#include <string>
#include <vector>

namespace atk
{
class DeviceIo2 : public atkAudioModule
{
public:
    DeviceIo2();
    ~DeviceIo2();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate) override;

    // When bypassed, DeviceIo2 does no processing and leaves the buffer untouched
    void setBypass(bool shouldBypass);
    bool isBypassed() const;

    // Set the fade time for bypass transitions (in seconds)
    void setFadeTime(double seconds);

    void setOutputDelay(float delayMs);
    float getOutputDelay() const;

    void setInputChannelMapping(const std::vector<std::vector<bool>>& mapping);
    std::vector<std::vector<bool>> getInputChannelMapping() const;

    void setOutputChannelMapping(const std::vector<std::vector<bool>>& mapping);
    std::vector<std::vector<bool>> getOutputChannelMapping() const;

    void getState(std::string& s) override;
    void setState(std::string& s) override;

    juce::Component* getWindowComponent() override;
    juce::Component* createEmbeddableSettingsComponent();

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk
