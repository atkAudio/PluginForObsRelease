#pragma once

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <atkaudio/ModuleInfrastructure/AudioServer/ChannelRoutingMatrix.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <atkaudio/SharedPluginList.h>
#include <juce_audio_utils/juce_audio_utils.h>

//==============================================================================
enum class EditorStyle
{
    thisWindow,
    newWindow
};

//==============================================================================
/**
 * Core audio processor implementation for hosting VST/AU/other plugins.
 * Handles plugin loading, audio/MIDI routing, and state management.
 */
class HostAudioProcessorImpl
    : public juce::AudioProcessor
    , private juce::ChangeListener
{
public:
    // Constructor that accepts channel count from OBS
    // If numChannels is 0, defaults to stereo (2 channels)
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

    /**
     * Get direct access to the loaded inner plugin instance.
     * Returns nullptr if no plugin is loaded.
     * Thread-safe access via internal mutex.
     */
    juce::AudioPluginInstance* getInnerPlugin() const;

    /**
     * Set channel mapping for input routing.
     * Controls which OBS (outer processor) channels pass through to inner plugin.
     * @param mapping 2D array [obsChannel][pluginChannel] = enabled
     */
    void setInputChannelMapping(const std::vector<std::vector<bool>>& mapping);

    /**
     * Get current input channel mapping
     */
    std::vector<std::vector<bool>> getInputChannelMapping() const;

    /**
     * Set channel mapping for output routing.
     * Controls which plugin output channels pass through to OBS output.
     * @param mapping 2D array [pluginChannel][obsChannel] = enabled
     */
    void setOutputChannelMapping(const std::vector<std::vector<bool>>& mapping);

    /**
     * Get current output channel mapping
     */
    std::vector<std::vector<bool>> getOutputChannelMapping() const;

    /**
     * Check if OBS sidechain input is enabled
     */
    bool isSidechainEnabled() const;

    /**
     * Enable/disable OBS sidechain input
     */
    void setSidechainEnabled(bool enabled);

    // Public members for UI access
    juce::ApplicationProperties appProperties;
    juce::AudioPluginFormatManager pluginFormatManager;

    // Own plugin list instance, loads from/saves to shared file
    juce::KnownPluginList pluginList;

    std::function<void()> pluginChanged;

    // MIDI and Audio clients
    atk::MidiClient midiClient;
    atk::AudioClient audioClient;

    // Multi-core processing callbacks (set by PluginHost)
    std::function<bool()> getMultiCoreEnabled;
    std::function<void(bool)> setMultiCoreEnabled;

    // Stats callbacks for CPU/latency display (set by PluginHost)
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

    // Channel routing matrix for managing input/output routing
    atk::ChannelRoutingMatrix routingMatrix;

    // Internal buffer for routing (OBS input → routing → plugin → routing → OBS output)
    juce::AudioBuffer<float> internalBuffer;

    // Device I/O buffers for applying routing matrix
    juce::AudioBuffer<float> deviceInputBuffer;  // One channel per input subscription
    juce::AudioBuffer<float> deviceOutputBuffer; // One channel per output subscription

    // Pre-allocated MIDI buffer for copying input MIDI (avoids allocation in audio path)
    juce::MidiBuffer inputMidiCopy;

    // OBS sidechain enabled state
    std::atomic<bool> sidechainEnabled{false};

    static constexpr const char* innerStateTag = "inner_state";
    static constexpr const char* editorStyleTag = "editor_style";

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostAudioProcessorImpl)
};

//==============================================================================
/**
 * Final processor class with editor support.
 */
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
