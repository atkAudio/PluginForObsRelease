#pragma once

#include "../Core/PluginGraph.h"
// Not using parallel graph implementation
// #include "../../AudioProcessorGraphMT/AudioProcessorGraphMT_Impl.h"

#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

class MainHostWindow;

class GraphEditorPanel final
    : public Component
    , public ChangeListener
    , private Timer
{
public:
    GraphEditorPanel(PluginGraph& graph);
    ~GraphEditorPanel() override;

    void createNewPlugin(const PluginDescriptionAndPreference&, Point<int> position);

    void paint(Graphics&) override;
    void resized() override;

    void mouseDown(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;

    void changeListenerCallback(ChangeBroadcaster*) override;

    void updateComponents();

    void showPopupMenu(Point<int> position);

    void beginConnectorDrag(
        AudioProcessorGraphMT::NodeAndChannel source,
        AudioProcessorGraphMT::NodeAndChannel dest,
        const MouseEvent&
    );
    void dragConnector(const MouseEvent&);
    void endDraggingConnector(const MouseEvent&);

    PluginGraph& graph;

private:
    struct PluginComponent;
    struct ConnectorComponent;
    struct PinComponent;

    OwnedArray<PluginComponent> nodes;
    OwnedArray<ConnectorComponent> connectors;
    std::unique_ptr<ConnectorComponent> draggingConnector;
    std::unique_ptr<PopupMenu> menu;

    PluginComponent* getComponentForPlugin(AudioProcessorGraphMT::NodeID) const;
    ConnectorComponent* getComponentForConnection(const AudioProcessorGraphMT::Connection&) const;
    PinComponent* findPinAt(Point<float>) const;

    Point<int> originalTouchPos;

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphEditorPanel)
};

class GraphDocumentComponent final
    : public Component
    , public DragAndDropTarget
    , public DragAndDropContainer
    , public Timer
    , private ChangeListener
    , private juce::MidiKeyboardStateListener
{
public:
    GraphDocumentComponent(
        MainHostWindow& mainHostWindow,
        AudioPluginFormatManager& formatManager,
        AudioDeviceManager& deviceManager,
        KnownPluginList& pluginList
    );

    ~GraphDocumentComponent() override;

    void setCpuLoad()
    {
        auto cpuLoad = graphAudioCallback ? graphAudioCallback->getCpuLoad() : 0.0f;

        auto latencySamples = graph->graph.getLatencySamples();
        auto latencyMs =
            latencySamples > 0
                ? (int)std::round(
                      (float)latencySamples / deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() * 1000.0f
                  )
                : 0;

        cpuLoadLabel.setText(
            "dly: "
                // + juce::String(latencySamples) + "smp/"
                + juce::String(latencyMs)
                + "ms, "
                + "cpu: "
                + juce::String(cpuLoad, 2).replace("0.", "."),
            juce::dontSendNotification
        );
    }

    void timerCallback() override
    {
        setCpuLoad();
    }

    void createNewPlugin(const PluginDescriptionAndPreference&, Point<int> position);
    bool closeAnyOpenPluginWindows();

    std::unique_ptr<PluginGraph> graph;

    void resized() override;
    void releaseGraph();

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

    std::unique_ptr<GraphEditorPanel> graphPanel;
    std::unique_ptr<MidiKeyboardComponent> keyboardComp;

    void showSidePanel(bool isSettingsPanel);
    void hideLastSidePanel();

    BurgerMenuComponent burgerMenu;

private:
    class GraphAudioCallback : public juce::AudioIODeviceCallback
    {
    public:
        GraphAudioCallback(GraphDocumentComponent& owner);
        ~GraphAudioCallback() override;

        void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
        void audioDeviceStopped() override;
        void audioDeviceIOCallbackWithContext(
            const float* const* inputChannelData,
            int numInputChannels,
            float* const* outputChannelData,
            int numOutputChannels,
            int numSamples,
            const juce::AudioIODeviceCallbackContext& context
        ) override;

        // Legacy callback for non-context aware systems (not virtual, but called by JUCE)
        void audioDeviceIOCallback(
            const float* const* inputChannelData,
            int numInputChannels,
            float* const* outputChannelData,
            int numOutputChannels,
            int numSamples
        );

    private:
        GraphDocumentComponent& owner;
        juce::CriticalSection callbackLock;
        double sampleRate = 44100.0;
        int blockSize = 512;
        bool isPrepared = false;
        juce::AudioIODevice* currentDevice = nullptr;
        juce::AudioProcessLoadMeasurer loadMeasurer;

        // Peak hold for CPU load display (3 second hold)
        mutable float peakCpuLoad = 0.0f;
        mutable double peakCpuTime = 0.0;

    public:
        float getCpuLoad() const
        {
            float currentLoad = static_cast<float>(loadMeasurer.getLoadAsProportion());

            // Peak hold for 3 seconds
            auto now = juce::Time::getMillisecondCounterHiRes();
            if (currentLoad >= peakCpuLoad)
            {
                peakCpuLoad = currentLoad;
                peakCpuTime = now;
            }
            else if (now - peakCpuTime > 3000.0)
            {
                peakCpuLoad = currentLoad;
                peakCpuTime = now;
            }

            return peakCpuLoad;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphAudioCallback)
    };

    Label cpuLoadLabel;

    AudioDeviceManager& deviceManager;
    KnownPluginList& pluginList;

    std::unique_ptr<GraphAudioCallback> graphAudioCallback;
    MidiKeyboardState keyState;
    MidiOutput* midiOutput = nullptr;

    MainHostWindow& mainHostWindow;

    struct TooltipBar;
    std::unique_ptr<TooltipBar> statusBar;

    class TitleBarComponent;
    std::unique_ptr<TitleBarComponent> titleBarComponent;

    struct PluginListBoxModel;
    std::unique_ptr<PluginListBoxModel> pluginListBoxModel;

    ListBox pluginListBox;

    SidePanel mobileSettingsSidePanel{"Settings", 300, true};
    SidePanel pluginListSidePanel{"Plugins", 250, false};
    SidePanel* lastOpenedSidePanel = nullptr;

    void changeListenerCallback(ChangeBroadcaster*) override;

    // MidiKeyboardStateListener interface - inject virtual keyboard MIDI into MidiServer
    void handleNoteOn(juce::MidiKeyboardState* source, int midiChannel, int midiNoteNumber, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState* source, int midiChannel, int midiNoteNumber, float velocity) override;

    void init();
    void checkAvailableWidth();
    void updateMidiOutput();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphDocumentComponent)
};
