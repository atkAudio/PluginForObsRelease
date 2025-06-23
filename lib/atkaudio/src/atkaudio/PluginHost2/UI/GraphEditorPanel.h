#pragma once

#include "../Plugins/PluginGraph.h"

#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

class MainHostWindow;

//==============================================================================
/**
    A panel that displays and edits a PluginGraph.
*/
class GraphEditorPanel final
    : public Component
    , public ChangeListener
    , private Timer
{
public:
    //==============================================================================
    GraphEditorPanel(PluginGraph& graph);
    ~GraphEditorPanel() override;

    void createNewPlugin(const PluginDescriptionAndPreference&, Point<int> position);

    void paint(Graphics&) override;
    void resized() override;

    void mouseDown(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;

    void changeListenerCallback(ChangeBroadcaster*) override;

    //==============================================================================
    void updateComponents();

    //==============================================================================
    void showPopupMenu(Point<int> position);

    //==============================================================================
    void beginConnectorDrag(
        AudioProcessorGraph::NodeAndChannel source,
        AudioProcessorGraph::NodeAndChannel dest,
        const MouseEvent&
    );
    void dragConnector(const MouseEvent&);
    void endDraggingConnector(const MouseEvent&);

    //==============================================================================
    PluginGraph& graph;

private:
    struct PluginComponent;
    struct ConnectorComponent;
    struct PinComponent;

    OwnedArray<PluginComponent> nodes;
    OwnedArray<ConnectorComponent> connectors;
    std::unique_ptr<ConnectorComponent> draggingConnector;
    std::unique_ptr<PopupMenu> menu;

    PluginComponent* getComponentForPlugin(AudioProcessorGraph::NodeID) const;
    ConnectorComponent* getComponentForConnection(const AudioProcessorGraph::Connection&) const;
    PinComponent* findPinAt(Point<float>) const;

    //==============================================================================
    Point<int> originalTouchPos;

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphEditorPanel)
};

//==============================================================================
/**
    A panel that embeds a GraphEditorPanel with a midi keyboard at the bottom.

    It also manages the graph itself, and plays it.
*/
class GraphDocumentComponent final
    : public Component
    , public DragAndDropTarget
    , public DragAndDropContainer
    , public Timer
    , private ChangeListener
{
public:
    GraphDocumentComponent(
        MainHostWindow& mainHostWindow,
        AudioPluginFormatManager& formatManager,
        AudioDeviceManager& deviceManager,
        KnownPluginList& pluginList
    );

    ~GraphDocumentComponent() override;

    float cpuLoadPrev = 0.0f;
    int cpuHold = 0;

    void setCpuLoad()
    {
        auto cpuLoad = deviceManager.getCpuUsage();
        if (cpuLoad > cpuLoadPrev)
        {
            cpuHold = 100;
        }
        else if (cpuHold > 0)
        {
            --cpuHold;
            cpuLoad = cpuLoadPrev;
        }
        cpuLoadPrev = cpuLoad;

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
                + juce::String(latencyMs) + "ms, " + "cpu: " + juce::String(cpuLoad, 2).replace("0.", "."),
            juce::dontSendNotification
        );
    }

    void timerCallback() override
    {
        setCpuLoad();
    }

    //==============================================================================
    void createNewPlugin(const PluginDescriptionAndPreference&, Point<int> position);
    void setDoublePrecision(bool doublePrecision);
    bool closeAnyOpenPluginWindows();

    //==============================================================================
    std::unique_ptr<PluginGraph> graph;

    void resized() override;
    void releaseGraph();

    //==============================================================================
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

    //==============================================================================
    std::unique_ptr<GraphEditorPanel> graphPanel;
    std::unique_ptr<MidiKeyboardComponent> keyboardComp;

    //==============================================================================
    void showSidePanel(bool isSettingsPanel);
    void hideLastSidePanel();

    BurgerMenuComponent burgerMenu;

private:
    Label cpuLoadLabel;
    //==============================================================================
    AudioDeviceManager& deviceManager;
    KnownPluginList& pluginList;

    AudioProcessorPlayer graphPlayer;
    MidiKeyboardState keyState;
    MidiOutput* midiOutput = nullptr;

    struct TooltipBar;
    std::unique_ptr<TooltipBar> statusBar;

    class TitleBarComponent;
    std::unique_ptr<TitleBarComponent> titleBarComponent;

    //==============================================================================
    struct PluginListBoxModel;
    std::unique_ptr<PluginListBoxModel> pluginListBoxModel;

    ListBox pluginListBox;

    SidePanel mobileSettingsSidePanel{"Settings", 300, true};
    SidePanel pluginListSidePanel{"Plugins", 250, false};
    SidePanel* lastOpenedSidePanel = nullptr;

    //==============================================================================
    void changeListenerCallback(ChangeBroadcaster*) override;

    void init();
    void checkAvailableWidth();
    void updateMidiOutput();

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphDocumentComponent)
};
