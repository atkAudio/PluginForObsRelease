#pragma once

#include "../PluginGraph.h"
#include "GraphViewport.h"
#include <map>

#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

class MainHostWindow;

namespace CommandIDs
{
static const int open = 0x30000;
static const int saveAs = 0x30002;
static const int newFile = 0x30003;
static const int showPluginListEditor = 0x30100;
static const int showAudioSettings = 0x30200;
static const int showMidiSettings = 0x30210;
static const int allWindowsForward = 0x30400;
static const int deleteSelection = 0x30700;
static const int disconnectSelection = 0x30701;
static const int toggleBypassSelection = 0x30702;
static const int groupSelection = 0x30703;
static const int ungroupSelection = 0x30704;
static const int renameGroup = 0x30705;
static const int saveNodeState = 0x30706;
static const int loadNodeState = 0x30707;
static const int saveGroup = 0x30708;
static const int loadGroupState = 0x30709;
static const int duplicateSelection = 0x3070a;
static const int zoomIn = 0x30800;
static const int zoomOut = 0x30801;
static const int resetZoom = 0x30802;
static const int zoomToFit = 0x30803;
} // namespace CommandIDs

class GraphEditorPanel final
    : public Component
    , public ChangeListener
    , public ApplicationCommandTarget
    , public LassoSource<AudioProcessorGraphMT::NodeID>
    , private Timer
{
public:
    using NodeID = AudioProcessorGraphMT::NodeID;
    GraphEditorPanel(PluginGraph& graph);
    ~GraphEditorPanel() override;

    void createNewPlugin(const PluginDescriptionAndPreference&, Point<int> position);

    void paint(Graphics&) override;

    void mouseDown(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseWheelMove(const MouseEvent&, const MouseWheelDetails&) override;
    bool keyPressed(const KeyPress&) override;

    void setGraphViewport(GraphViewport* view)
    {
        graphViewport = view;
    }

    Point<int> getLayoutOrigin() const
    {
        return layoutOrigin;
    }

    juce::Rectangle<int> getGraphVisualBounds() const
    {
        return graphVisualBounds;
    }

    ApplicationCommandTarget* getNextCommandTarget() override;
    void getAllCommands(Array<CommandID>&) override;
    void getCommandInfo(CommandID, ApplicationCommandInfo&) override;
    bool perform(const InvocationInfo&) override;
    void findLassoItemsInArea(Array<NodeID>&, const juce::Rectangle<int>&) override;
    SelectedItemSet<NodeID>& getLassoSelection() override;
    void showSelectionMenu(Point<int> screenPosition);
    void addSelectionItemsToMenu(PopupMenu&, std::optional<Point<int>> insertPosition = {});

    void changeListenerCallback(ChangeBroadcaster*) override;

    void updateComponents();

    ApplicationCommandManager& getCommandManager();

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
    struct GroupComponent;

    OwnedArray<PluginComponent> nodes;
    OwnedArray<ConnectorComponent> connectors;
    OwnedArray<GroupComponent> groupFrames;
    std::unique_ptr<ConnectorComponent> draggingConnector;
    std::unique_ptr<PopupMenu> menu;
    SelectedItemSet<NodeID> selection;
    LassoComponent<NodeID> lasso;
    std::map<NodeID, Point<double>> dragStartPositions;
    std::unique_ptr<PluginGraph::Edit> dragEdit, connectorEdit;
    std::optional<AudioProcessorGraphMT::Connection> originalConnection;
    std::unique_ptr<FileChooser> fileChooser;
    std::unique_ptr<AlertWindow> renameWindow;
    ScopedMessageBox messageBox;
    ApplicationCommandManager localCommandManager;

    PluginComponent* getComponentForPlugin(AudioProcessorGraphMT::NodeID) const;
    ConnectorComponent* getComponentForConnection(const AudioProcessorGraphMT::Connection&) const;
    PinComponent* findPinAt(Point<float>) const;
    GroupComponent* getComponentForGroup(uint32) const;
    GroupComponent* getCollapsedGroupFor(NodeID) const;
    Point<float> getEndpointAnchor(NodeID, int, bool, Point<float>) const;
    Point<float> getVisibleEndpoint(NodeID, int, bool, Point<float>, Point<float>) const;
    Array<NodeID> getSelectedNodes() const;
    std::optional<NodeID> getSingleSelectedNode() const;
    uint32 getSelectedGroupId() const;
    void selectOnly(const Array<NodeID>&);
    void beginDraggingNodes(const Array<NodeID>&, const MouseEvent&);
    void dragNodes(const MouseEvent&);
    void endDraggingNodes(bool);
    Point<double> toNormalised(Point<int>) const;
    Point<double> getPasteOffset() const;
    void copySelection();
    void pasteFromClipboard();
    void duplicateSelection();
    void insertSnippet(const XmlElement&, Point<double>);
    void deleteSelection();
    void toggleBypassOfSelection();
    void showRenameGroupWindow(uint32);
    void showRenameNodeWindow(NodeID);
    void showNodeWindow(NodeID, PluginWindow::Type);
    void chooseFile(const String&, const String&, bool, std::function<Result(const File&)>);
    void showError(const String&, const Result&);
    void cancelGestures(bool documentReplaced);

    Point<int> originalTouchPos;
    Point<float> nodeDragStartScreenPosition;
    GraphViewport* graphViewport = nullptr;
    Point<int> layoutOrigin;
    juce::Rectangle<int> graphVisualBounds;
    uint64 documentGeneration = 0;
    enum class BackgroundGesture
    {
        none,
        pan,
        lasso
    };
    BackgroundGesture backgroundGesture = BackgroundGesture::none;
    Point<int> visibleCentre() const;
    void updateLayoutBounds();

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

    PluginGraph* graph = nullptr;

    void resized() override;
    void releaseGraph();

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

    std::unique_ptr<GraphEditorPanel> graphPanel;
    std::unique_ptr<GraphViewport> graphViewport;
    std::unique_ptr<MidiKeyboardComponent> keyboardComp;

private:
    Label cpuLoadLabel;

    AudioDeviceManager& deviceManager;
    KnownPluginList& pluginList;

    MidiKeyboardState keyState;

    MainHostWindow& mainHostWindow;

    struct TooltipBar;
    std::unique_ptr<TooltipBar> statusBar;

    void changeListenerCallback(ChangeBroadcaster*) override;

    // MidiKeyboardStateListener interface - inject virtual keyboard MIDI into MidiServer
    void handleNoteOn(juce::MidiKeyboardState* source, int midiChannel, int midiNoteNumber, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState* source, int midiChannel, int midiNoteNumber, float velocity) override;

    void init();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphDocumentComponent)
};
