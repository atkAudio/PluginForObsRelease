#pragma once

#include "../atkAudioModule.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/ChannelRoutingMatrix.h>
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleBridge.h>

#include <atomic>
#include <juce_dsp/juce_dsp.h>
#include <memory>
#include <string>
#include <vector>

namespace atk
{
class DeviceIo2
    : public atkAudioModule
    , private juce::AsyncUpdater
{
public:
    struct WrapperControlState
    {
        bool followSourceVolume = false;
        bool followScene = false;
        float inputGainDb = 0.0f;
        float outputGainDb = 0.0f;
        float sourceVolume = 1.0f;
        bool sourceMuted = false;
        double fadeTimeSeconds = 0.5;
        bool hasDesiredBypass = false;
        bool desiredBypass = true;
    };

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

    void setWrapperControlState(const WrapperControlState& state);

    void getState(std::string& s) override;
    void setState(std::string& s) override;

    juce::Component* getWindowComponent() override;
    juce::Component* createEmbeddableSettingsComponent();

private:
    enum class UpdateType
    {
        None,
        ChannelInfo,
        InputBufferResize,
        OutputBufferResize
    };

    void handleAsyncUpdate() override;
    void updateChannelInfoOnMessageThread(int numChannels);
    void prepareOutputDelay(int numChannels, int numSamples, double sampleRate);
    void applyOutputDelay(juce::AudioBuffer<float>& buffer, int numChannels, int numSamples, double sampleRate);

    atk::AudioClient audioClient;
    atk::ChannelRoutingMatrix routingMatrix;

    std::unique_ptr<juce::AudioDeviceManager> deviceManager;
    std::unique_ptr<atk::ModuleDeviceManager> moduleDeviceManager;

    juce::AudioBuffer<float> deviceInputBuffer;
    juce::AudioBuffer<float> deviceOutputBuffer;
    juce::AudioBuffer<float> internalBuffer;
    std::vector<float*> fadeDeviceOutputPointers;

    std::unique_ptr<juce::DocumentWindow> settingsWindow;

    int currentNumChannels = 2;
    int preparedNumChannels = 0;
    int preparedNumSamples = 0;
    double preparedSampleRate = 0.0;

    std::atomic<float> outputDelayMs{0.0f};
    std::vector<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>> outputDelayLines;
    std::vector<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>> outputDelaySmooth;
    bool delayPrepared = false;

    std::atomic<bool> bypass{false};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> fadeGain{1.0f};
    std::atomic<double> fadeDurationSeconds{0.5};

    std::atomic<bool> followSourceVolume{false};
    std::atomic<bool> followScene{false};
    std::atomic<float> inputGainDb{0.0f};
    std::atomic<float> outputGainDb{0.0f};
    std::atomic<float> sourceVolume{1.0f};
    std::atomic<bool> sourceMuted{false};
    std::atomic<bool> hasDesiredBypass{false};
    std::atomic<bool> desiredBypass{true};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGainSmooth{1.0f};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> inputGainSmooth{1.0f};
    double gainSmoothingSampleRate = 0.0;

    std::atomic<UpdateType> pendingUpdateType{UpdateType::None};
    std::atomic<int> pendingNumChannels{0};
    std::atomic<int> pendingNumSamples{0};
    std::atomic<int> pendingInputSubs{0};
    std::atomic<int> pendingOutputSubs{0};
};
} // namespace atk
