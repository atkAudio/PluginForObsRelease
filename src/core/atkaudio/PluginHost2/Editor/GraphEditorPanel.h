#pragma once

#include "../PluginGraph.h"
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
        PluginGraph& graphModel,
        AudioDeviceManager& deviceManager,
        KnownPluginList& pluginList
    );

    ~GraphDocumentComponent() override;

    void setCpuLoad();

    void timerCallback() override
    {
        setCpuLoad();
    }

    void createNewPlugin(const PluginDescriptionAndPreference&, Point<int> position);
    bool closeAnyOpenPluginWindows();

    PluginGraph* graph = nullptr;

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
    Label cpuLoadLabel;

    AudioDeviceManager& deviceManager;
    KnownPluginList& pluginList;

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
