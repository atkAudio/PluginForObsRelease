#pragma once

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <atkaudio/ModuleInfrastructure/AudioServer/ChannelRoutingMatrix.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <atkaudio/SharedPluginList.h>
#include <juce_audio_utils/juce_audio_utils.h>

enum class EditorStyle
{
    thisWindow,
    newWindow
};

class HostAudioProcessorImpl
    : public juce::AudioProcessor
    , private juce::ChangeListener
{
public:
    explicit HostAudioProcessorImpl(int numChannels = 2);
    ~HostAudioProcessorImpl() override;

    // AudioProcessor interface
    bool isBusesLayoutSupported(const juce::AudioProcessor::BusesLayout& layouts) const final;
    void prepareToPlay(double sampleRate, int samplesPerBlock) final;
    void releaseResources() final;
    void reset() final;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiBuffer) final;
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) final;

    bool hasEditor() const override;
    juce::AudioProcessorEditor* createEditor() override;

    const juce::String getName() const final;
    bool acceptsMidi() const final;
    bool producesMidi() const final;
    double getTailLengthSeconds() const final;

    int getNumPrograms() final;
    int getCurrentProgram() final;
    void setCurrentProgram(int index) final;
    const juce::String getProgramName(int index) final;
    void changeProgramName(int index, const juce::String& newName) final;

    void getStateInformation(juce::MemoryBlock& destData) final;
    void setStateInformation(const void* data, int sizeInBytes) final;

    // Plugin management
    void setNewPlugin(const juce::PluginDescription& pd, EditorStyle where, const juce::MemoryBlock& mb = {});
    void clearPlugin();
    bool isPluginLoaded() const;
    std::unique_ptr<juce::AudioProcessorEditor> createInnerEditor() const;
    EditorStyle getEditorStyle() const noexcept;

    juce::AudioPluginInstance* getInnerPlugin() const;

    void setInputChannelMapping(const std::vector<std::vector<bool>>& mapping);

    std::vector<std::vector<bool>> getInputChannelMapping() const;

    void setOutputChannelMapping(const std::vector<std::vector<bool>>& mapping);

    std::vector<std::vector<bool>> getOutputChannelMapping() const;

    // Public members for UI access
    juce::ApplicationProperties appProperties;
    juce::AudioPluginFormatManager pluginFormatManager;

    // Own plugin list instance, loads from/saves to shared file
    juce::KnownPluginList pluginList;

    std::function<void()> pluginChanged;

    atk::MidiClient midiClient;
    atk::AudioClient audioClient;

    std::function<bool()> getMultiCoreEnabled;
    std::function<void(bool)> setMultiCoreEnabled;

    std::function<float()> getCpuLoad;
    std::function<int()> getLatencyMs;

private:
    class AtkAudioPlayHead : public juce::AudioPlayHead
    {
    public:
        juce::AudioPlayHead::PositionInfo positionInfo;
        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override;
    };

    static juce::AudioChannelSet getChannelSetForCount(int numChannels);
    void changeListenerCallback(juce::ChangeBroadcaster* source) final;

    static inline juce::InterProcessLock appPropertiesLock{"atkAudioPluginHostLock"};

    juce::CriticalSection innerMutex;
    std::unique_ptr<juce::AudioPluginInstance> inner;
    EditorStyle editorStyle = EditorStyle::thisWindow;
    bool active = false;
    juce::ScopedMessageBox messageBox;
    AtkAudioPlayHead atkPlayHead;

    atk::ChannelRoutingMatrix routingMatrix;

    juce::AudioBuffer<float> internalBuffer;

    juce::AudioBuffer<float> deviceInputBuffer;
    juce::AudioBuffer<float> deviceOutputBuffer;

    juce::MidiBuffer inputMidiCopy;

    static constexpr const char* innerStateTag = "inner_state";
    static constexpr const char* editorStyleTag = "editor_style";

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostAudioProcessorImpl)
};

class HostAudioProcessor final : public HostAudioProcessorImpl
{
public:
    HostAudioProcessor()
        : HostAudioProcessorImpl()
    {
    }

    bool hasEditor() const override;
    juce::AudioProcessorEditor* createEditor() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostAudioProcessor)
};
