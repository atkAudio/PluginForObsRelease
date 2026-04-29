#pragma once

#include "../../atkAudioModule.h"

#include <memory>
#include <string>

class MainHostWindow;

namespace atk
{
class ModuleDeviceManager;

class PluginHost2
    : public atkAudioModule
    , private juce::AsyncUpdater
{
public:
    PluginHost2();
    ~PluginHost2();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate) override;

    // WindowModule interface
    void getState(std::string& s) override;
    void setState(std::string& s) override;

    // Set the parent OBS source (extracts UUID for filtering)
    void setParentSource(void* parentSource);

protected:
    // AudioModule interface - only need to provide the window component
    juce::Component* getWindowComponent() override;

private:
    void handleAsyncUpdate() override;

    std::unique_ptr<MainHostWindow> mainHostWindow;
    std::unique_ptr<ModuleDeviceManager> moduleDeviceManager;
    juce::String pendingStateString;
};
} // namespace atk