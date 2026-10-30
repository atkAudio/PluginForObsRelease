#include "GraphEditorPanel.h"
#include "../missing_plugin.h"

#include "../../Logging.h"
#include <set>

struct GraphEditorPanel::PinComponent final
    : public Component
    , public SettableTooltipClient
{
    PinComponent(GraphEditorPanel& p, AudioProcessorGraphMT::NodeAndChannel pinToUse, bool isIn)
        : panel(p)
        , graph(p.graph)
        , pin(pinToUse)
        , isInput(isIn)
    {
        if (auto node = graph.graph.getNodeForId(pin.nodeID))
        {
            String tip;

            if (pin.isMIDI())
            {
                tip = isInput ? "MIDI Input" : "MIDI Output";
            }
            else
            {
                auto& processor = *node->getProcessor();

                // Check if this is an Audio Input or Audio Output node FIRST
                // (before checking for buses, as these nodes have default buses but we want custom names)
                if (auto* ioProcessor = dynamic_cast<AudioProcessorGraphMT::AudioGraphIOProcessor*>(&processor))
                {
                    auto ioType = ioProcessor->getType();
                    if (ioType == AudioProcessorGraphMT::AudioGraphIOProcessor::audioInputNode
                        || ioType == AudioProcessorGraphMT::AudioGraphIOProcessor::audioOutputNode)
                    {
                        // Try to get actual channel name from the current audio device
                        auto* host = graph.getNativeEditorHost();
                        auto* device = host != nullptr ? host->getDeviceManager().getCurrentAudioDevice() : nullptr;
                        if (device != nullptr)
                        {
                            auto channelNames = ioType == AudioProcessorGraphMT::AudioGraphIOProcessor::audioInputNode
                                                  ? device->getInputChannelNames()
                                                  : device->getOutputChannelNames();

                            if (pin.channelIndex < channelNames.size())
                                tip = String(pin.channelIndex + 1) + ": " + channelNames[pin.channelIndex];
                            else
                                tip = String(pin.channelIndex + 1);
                        }
                        else
                        {
                            tip = String(pin.channelIndex + 1);
                        }
                    }
                    else
                    {
                        tip = (isInput ? "MIDI Input" : "MIDI Output");
                    }
                }
                else
                {
                    // For regular plugins, check if they have buses
                    auto channel =
                        processor.getOffsetInBusBufferForAbsoluteChannelIndex(isInput, pin.channelIndex, busIdx);

                    if (auto* bus = processor.getBus(isInput, busIdx))
                        tip = bus->getName()
                            + ": "
                            + AudioChannelSet::getAbbreviatedChannelTypeName(
                                  bus->getCurrentLayout().getTypeOfChannel(channel)
                            );
                    else
                        tip = (isInput ? "Main Input: " : "Main Output: ") + String(pin.channelIndex + 1);
                }
            }

            setTooltip(tip);
        }

        setSize(16, 16);
    }

    void paint(Graphics& g) override
    {
        auto w = (float)getWidth();
        auto h = (float)getHeight();

        Path p;
        p.addEllipse(w * 0.25f, h * 0.25f, w * 0.5f, h * 0.5f);
        p.addRectangle(w * 0.4f, isInput ? (0.5f * h) : 0.0f, w * 0.2f, h * 0.5f);

        auto colour = (pin.isMIDI() ? Colours::red : Colours::green);

        g.setColour(colour.withRotatedHue((float)busIdx / 5.0f));
        g.fillPath(p);
    }

    void mouseDown(const MouseEvent& e) override
    {
        panel.cancelGestures(panel.documentGeneration != graph.getDocumentGeneration());
        AudioProcessorGraphMT::NodeAndChannel dummy{{}, 0};

        panel.beginConnectorDrag(isInput ? dummy : pin, isInput ? pin : dummy, e);
    }

    void mouseDrag(const MouseEvent& e) override
    {
        panel.dragConnector(e);
    }

    void mouseUp(const MouseEvent& e) override
    {
        panel.endDraggingConnector(e);
    }

    GraphEditorPanel& panel;
    PluginGraph& graph;
    AudioProcessorGraphMT::NodeAndChannel pin;
    const bool isInput;
    int busIdx = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PinComponent)
};

struct GraphEditorPanel::PluginComponent final
    : public Component
    , public SettableTooltipClient
    , public Timer
    , private AudioProcessorParameter::Listener
    , private AsyncUpdater
{
    PluginComponent(GraphEditorPanel& p, AudioProcessorGraphMT::NodeID id)
        : panel(p)
        , graph(p.graph)
        , pluginID(id)
        , processorNode(graph.graph.getNodeForId(id))
    {
        shadow = DropShadow(Colours::black.withAlpha(0.5f), 3, {0, 1});

        if (auto f = graph.graph.getNodeForId(pluginID))
        {
            if (auto* processor = f->getProcessor())
            {
                if (auto* bypassParam = processor->getBypassParameter())
                    bypassParam->addListener(this);
            }
        }

        setSize(150, 60);
    }

    PluginComponent(const PluginComponent&) = delete;
    PluginComponent& operator=(const PluginComponent&) = delete;

    ~PluginComponent() override
    {
        if (auto f = processorNode)
        {
            if (auto* processor = f->getProcessor())
            {
                if (auto* bypassParam = processor->getBypassParameter())
                    bypassParam->removeListener(this);
            }
        }
    }

    void mouseDown(const MouseEvent& e) override
    {
        toFront(true);
        if (e.mods.isPopupMenu())
        {
            if (!panel.selection.isSelected(pluginID))
                panel.selection.selectOnly(pluginID);
            panel.showSelectionMenu(e.getScreenPosition());
            return;
        }
        selectionResultOnMouseDown = panel.selection.addToSelectionOnMouseDown(pluginID, e.mods);
        panel.beginDraggingNodes(panel.getSelectedNodes(), e);
        if (e.source.isTouch())
        {
            originalPos = e.position.toInt();
            startTimer(750);
        }
    }

    void mouseDrag(const MouseEvent& e) override
    {
        if (e.source.isTouch() && e.getDistanceFromDragStart() > 5)
            stopTimer();
        if (!e.mods.isPopupMenu() && panel.selection.isSelected(pluginID))
            panel.dragNodes(e);
    }

    void mouseUp(const MouseEvent& e) override
    {
        stopTimer();
        if (e.mods.isPopupMenu())
            return;
        bool wasDragged = e.mouseWasDraggedSinceMouseDown();
        panel.selection.addToSelectionOnMouseUp(pluginID, e.mods, wasDragged, selectionResultOnMouseDown);
        panel.endDraggingNodes(wasDragged);
        if (!wasDragged && e.getNumberOfClicks() == 2)
            panel.showNodeWindow(pluginID, PluginWindow::Type::normal);
    }

    bool hitTest(int x, int y) override
    {
        for (auto* child : getChildren())
            if (child->getBounds().contains(x, y))
                return true;

        return x >= 3 && x < getWidth() - 6 && y >= pinSize && y < getHeight() - pinSize;
    }

    void paint(Graphics& g) override
    {
        auto boxArea = getLocalBounds().reduced(4, pinSize);
        shadow.drawForRectangle(g, boxArea);
        bool isBypassed = false;
        String displayName = getName();

        if (auto* f = graph.graph.getNodeForId(pluginID))
        {
            isBypassed = f->isBypassed();

            // Use custom name if set
            auto customName = f->properties["customName"].toString();
            if (customName.isNotEmpty())
                displayName = customName + formatSuffix;
        }

        auto boxColour = findColour(TextEditor::backgroundColourId);

        if (isBypassed)
            boxColour = boxColour.brighter();

        g.setColour(boxColour);
        g.fillRect(boxArea.toFloat());

        if (panel.selection.isSelected(pluginID))
        {
            g.setColour(findColour(TextEditor::focusedOutlineColourId));
            g.drawRect(boxArea.toFloat(), 2.0f);
        }

        g.setColour(findColour(TextEditor::textColourId));
        g.setFont(font);
        if (dynamic_cast<MissingPlugin*>(getProcessor()) != nullptr)
        {
            auto status = boxArea.removeFromBottom(16);
            g.setColour(Colours::darkorange);
            g.drawText("Missing", status, Justification::centred);
            g.setColour(findColour(TextEditor::textColourId));
        }
        g.drawFittedText(displayName, boxArea, Justification::centred, 2);
    }

    void resized() override
    {
        if (auto f = graph.graph.getNodeForId(pluginID))
        {
            if (auto* processor = f->getProcessor())
            {
                for (auto* pin : pins)
                {
                    const bool isInput = pin->isInput;
                    auto channelIndex = pin->pin.channelIndex;
                    int busIdx = 0;
                    processor->getOffsetInBusBufferForAbsoluteChannelIndex(isInput, channelIndex, busIdx);

                    const int total = isInput ? numIns : numOuts;
                    const int index = pin->pin.isMIDI() ? (total - 1) : channelIndex;

                    auto totalSpaces = static_cast<float>(total)
                                     + (static_cast<float>(jmax(0, processor->getBusCount(isInput) - 1)) * 0.5f);
                    auto indexPos = static_cast<float>(index) + (static_cast<float>(busIdx) * 0.5f);

                    pin->setBounds(
                        proportionOfWidth((1.0f + indexPos) / (totalSpaces + 1.0f)) - pinSize / 2,
                        pin->isInput ? 0 : (getHeight() - pinSize),
                        pinSize,
                        pinSize
                    );
                }
            }
        }
    }

    Point<float> getPinPos(int index, bool isInput) const
    {
        for (auto* pin : pins)
            if (pin->pin.channelIndex == index && isInput == pin->isInput)
                return getPosition().toFloat() + pin->getBounds().getCentre().toFloat();

        return {};
    }

    void update()
    {
        const AudioProcessorGraphMT::Node::Ptr f(graph.graph.getNodeForId(pluginID));
        jassert(f != nullptr);

        auto& processor = *f->getProcessor();

        numIns = processor.getTotalNumInputChannels();
        if (processor.acceptsMidi())
            ++numIns;

        numOuts = processor.getTotalNumOutputChannels();
        if (processor.producesMidi())
            ++numOuts;

        auto* missing = dynamic_cast<MissingPlugin*>(&processor);
        int w = 100;
        int h = missing != nullptr ? 80 : 60;

        w = jmax(w, (jmax(numIns, numOuts) + 1) * 20);

        const auto textWidth = GlyphArrangement::getStringWidthInt(font, processor.getName());
        w = jmax(w, 16 + jmin(textWidth, 300));
        if (textWidth > 300)
            h = missing != nullptr ? 116 : 100;

        setSize(w, h);
        setName(processor.getName() + formatSuffix);
        setTooltip(
            missing != nullptr ? "Missing: "
                                     + missing->description.name
                                     + "\n"
                                     + missing->description.pluginFormatName
                                     + "\n"
                                     + missing->description.fileOrIdentifier
                                     + "\n"
                                     + missing->failureReason
                               : String{}
        );

        {
            auto p = graph.getNodePosition(pluginID);
            setCentrePosition((GraphViewGeometry::canvasPosition(p) + panel.getLayoutOrigin().toDouble()).roundToInt());
        }

        if (numIns != numInputs || numOuts != numOutputs)
        {
            numInputs = numIns;
            numOutputs = numOuts;

            pins.clear();

            for (int i = 0; i < processor.getTotalNumInputChannels(); ++i)
                addAndMakeVisible(pins.add(new PinComponent(panel, {pluginID, i}, true)));

            if (processor.acceptsMidi())
                addAndMakeVisible(
                    pins.add(new PinComponent(panel, {pluginID, AudioProcessorGraphMT::midiChannelIndex}, true))
                );

            for (int i = 0; i < processor.getTotalNumOutputChannels(); ++i)
                addAndMakeVisible(pins.add(new PinComponent(panel, {pluginID, i}, false)));

            if (processor.producesMidi())
                addAndMakeVisible(
                    pins.add(new PinComponent(panel, {pluginID, AudioProcessorGraphMT::midiChannelIndex}, false))
                );

            resized();
        }
    }

    AudioProcessor* getProcessor() const
    {
        if (auto node = graph.graph.getNodeForId(pluginID))
            return node->getProcessor();

        return {};
    }

    void timerCallback() override
    {
        stopTimer();
        if (!panel.selection.isSelected(pluginID))
            panel.selection.selectOnly(pluginID);
        panel.showSelectionMenu(localPointToGlobal(originalPos));
    }

    void parameterValueChanged(int, float) override
    {
        // Parameter changes might come from the audio thread or elsewhere, but
        // we can only call repaint from the message thread.
        triggerAsyncUpdate();
    }

    void parameterGestureChanged(int, bool) override
    {
    }

    void handleAsyncUpdate() override
    {
        repaint();
    }

    GraphEditorPanel& panel;
    PluginGraph& graph;
    const AudioProcessorGraphMT::NodeID pluginID;
    AudioProcessorGraphMT::Node::Ptr processorNode;
    OwnedArray<PinComponent> pins;
    int numInputs = 0, numOutputs = 0;
    int pinSize = 16;
    Point<int> originalPos;
    bool selectionResultOnMouseDown = false;
    Font font = FontOptions{13.0f, Font::bold};
    int numIns = 0, numOuts = 0;
    DropShadow shadow;
    const String formatSuffix = getFormatSuffix(getProcessor());
};

struct GraphEditorPanel::ConnectorComponent final
    : public Component
    , public SettableTooltipClient
{
    explicit ConnectorComponent(GraphEditorPanel& p)
        : panel(p)
        , graph(p.graph)
    {
        setComponentID("connection");
    }

    void setInput(AudioProcessorGraphMT::NodeAndChannel newSource)
    {
        if (connection.source != newSource)
        {
            connection.source = newSource;
            update();
        }
    }

    void setOutput(AudioProcessorGraphMT::NodeAndChannel newDest)
    {
        if (connection.destination != newDest)
        {
            connection.destination = newDest;
            update();
        }
    }

    void dragStart(Point<float> pos)
    {
        lastInputPos = pos;
        resizeToFit();
    }

    void dragEnd(Point<float> pos)
    {
        lastOutputPos = pos;
        resizeToFit();
    }

    void update()
    {
        Point<float> p1, p2;
        getPoints(p1, p2);

        if (lastInputPos != p1 || lastOutputPos != p2)
            resizeToFit();
    }

    void resizeToFit()
    {
        juce::Point<float> p1, p2;
        getPoints(p1, p2);

        auto newBounds = juce::Rectangle<float>(p1, p2).expanded(4.0f).getSmallestIntegerContainer();

        if (newBounds != getBounds())
            setBounds(newBounds);
        else
            resized();

        repaint();
    }

    void getPoints(Point<float>& p1, Point<float>& p2) const
    {
        auto source =
            panel.getEndpointAnchor(connection.source.nodeID, connection.source.channelIndex, true, lastInputPos);
        auto destination = panel.getEndpointAnchor(
            connection.destination.nodeID,
            connection.destination.channelIndex,
            false,
            lastOutputPos
        );
        p1 = panel.getVisibleEndpoint(
            connection.source.nodeID,
            connection.source.channelIndex,
            true,
            destination,
            source
        );
        p2 = panel.getVisibleEndpoint(
            connection.destination.nodeID,
            connection.destination.channelIndex,
            false,
            source,
            destination
        );
    }

    void paint(Graphics& g) override
    {
        if (connection.source.isMIDI() || connection.destination.isMIDI())
            g.setColour(Colours::red);
        else
            g.setColour(Colours::green);

        g.fillPath(linePath);
    }

    bool hitTest(int x, int y) override
    {
        auto pos = Point<int>(x, y).toFloat();

        if (hitPath.contains(pos))
        {
            double distanceFromStart, distanceFromEnd;
            getDistancesFromEnds(pos, distanceFromStart, distanceFromEnd);

            // avoid clicking the connector when over a pin
            return distanceFromStart > 7.0 && distanceFromEnd > 7.0;
        }

        return false;
    }

    void mouseDown(const MouseEvent&) override
    {
        panel.cancelGestures(panel.documentGeneration != graph.getDocumentGeneration());
        dragging = false;
    }

    void mouseDrag(const MouseEvent& e) override
    {
        if (dragging)
        {
            panel.dragConnector(e);
        }
        else if (e.mouseWasDraggedSinceMouseDown())
        {
            if (panel.documentGeneration != graph.getDocumentGeneration())
                return;
            dragging = true;

            panel.connectorEdit = std::make_unique<PluginGraph::Edit>(graph, "Move connection");
            panel.originalConnection = connection;
            graph.removeConnection(connection);

            double distanceFromStart, distanceFromEnd;
            getDistancesFromEnds(getPosition().toFloat() + e.position, distanceFromStart, distanceFromEnd);
            const bool isNearerSource = (distanceFromStart < distanceFromEnd);

            AudioProcessorGraphMT::NodeAndChannel dummy{{}, 0};

            panel.beginConnectorDrag(
                isNearerSource ? dummy : connection.source,
                isNearerSource ? connection.destination : dummy,
                e
            );
        }
    }

    void mouseUp(const MouseEvent& e) override
    {
        if (dragging)
            panel.endDraggingConnector(e);
    }

    void resized() override
    {
        Point<float> p1, p2;
        getPoints(p1, p2);

        lastInputPos = p1;
        lastOutputPos = p2;

        p1 -= getPosition().toFloat();
        p2 -= getPosition().toFloat();

        linePath.clear();
        linePath.startNewSubPath(p1);
        linePath.cubicTo(p1.x, p1.y + (p2.y - p1.y) * 0.33f, p2.x, p1.y + (p2.y - p1.y) * 0.66f, p2.x, p2.y);

        PathStrokeType wideStroke(8.0f);
        wideStroke.createStrokedPath(hitPath, linePath);

        PathStrokeType stroke(2.5f);
        stroke.createStrokedPath(linePath, linePath);

        auto arrowW = 5.0f;
        auto arrowL = 4.0f;

        Path arrow;
        arrow.addTriangle(-arrowL, arrowW, -arrowL, -arrowW, arrowL, 0.0f);

        arrow.applyTransform(
            AffineTransform()
                .rotated(MathConstants<float>::halfPi - (float)atan2(p2.x - p1.x, p2.y - p1.y))
                .translated((p1 + p2) * 0.5f)
        );

        linePath.addPath(arrow);
        linePath.setUsingNonZeroWinding(true);
    }

    void getDistancesFromEnds(Point<float> p, double& distanceFromStart, double& distanceFromEnd) const
    {
        Point<float> p1, p2;
        getPoints(p1, p2);

        distanceFromStart = p1.getDistanceFrom(p);
        distanceFromEnd = p2.getDistanceFrom(p);
    }

    GraphEditorPanel& panel;
    PluginGraph& graph;
    AudioProcessorGraphMT::Connection connection{
        {{}, 0},
        {{}, 0}
    };
    Point<float> lastInputPos, lastOutputPos;
    Path linePath, hitPath;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ConnectorComponent)
};

struct GraphEditorPanel::GroupComponent final : public Component
{
    GroupComponent(GraphEditorPanel& p, uint32 id)
        : panel(p)
        , groupId(id)
    {
        setComponentID("group:" + String(id));
    }

    void update(const NodeGroup& group)
    {
        juce::Rectangle<int> area;
        for (auto uid : panel.graph.getGroups().getNodeUids(groupId))
            if (auto* component = panel.getComponentForPlugin(NodeID(uid)))
                area = area.isEmpty() ? component->getBounds() : area.getUnion(component->getBounds());
        name = group.name;
        bypassed = group.bypassed;
        collapsed = group.collapsed;
        if (collapsed)
            setBounds(juce::Rectangle<int>(180, 58).withCentre(area.getCentre()));
        else
        {
            area = {};
            for (auto& member : group.members)
                if (auto* component = panel.getComponentForPlugin(NodeID(member.uid)))
                    area = area.isEmpty() ? component->getBounds() : area.getUnion(component->getBounds());
            for (auto& child : group.childGroups)
                if (auto* frame = panel.getComponentForGroup(child.groupId))
                    area = area.isEmpty() ? frame->getBounds() : area.getUnion(frame->getBounds());
            setBounds(area.expanded(10).withTrimmedTop(-titleHeight));
        }
        repaint();
    }

    Array<NodeID> getMembers() const
    {
        Array<NodeID> members;
        for (auto uid : panel.graph.getGroups().getNodeUids(groupId))
            members.add(NodeID(uid));
        return members;
    }

    bool hitTest(int x, int y) override
    {
        return collapsed || y < titleHeight || x < 6 || x >= getWidth() - 6 || y >= getHeight() - 6;
    }

    void paint(Graphics& graphics) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        auto accent = findColour(TextEditor::focusedOutlineColourId);
        auto text = findColour(TextEditor::textColourId);
        bool selected = panel.getSelectedGroupId() == groupId;
        float alpha = bypassed ? 0.5f : 1.0f;
        graphics.setColour(accent.withAlpha(0.08f * alpha));
        graphics.fillRoundedRectangle(bounds, 6.0f);
        graphics.setColour((selected ? accent : text.withAlpha(0.4f)).withMultipliedAlpha(alpha));
        graphics.drawRoundedRectangle(bounds, 6.0f, selected ? 2.0f : 1.0f);
        graphics.setColour(text.withMultipliedAlpha(alpha));
        graphics.setFont(FontOptions{13.0f, Font::bold});
        auto titleBounds = getLocalBounds().removeFromTop(titleHeight);
        titleBounds.removeFromRight(disclosureWidth);
        graphics.drawText(
            name + (bypassed ? " (bypassed)" : ""),
            titleBounds.reduced(8, 0),
            Justification::centredLeft,
            true
        );
        Path disclosure;
        auto centre = getLocalBounds().removeFromTop(titleHeight).removeFromRight(20).toFloat().getCentre();
        if (collapsed)
            disclosure.addTriangle(
                centre.x - 2.5f,
                centre.y - 4.0f,
                centre.x - 2.5f,
                centre.y + 4.0f,
                centre.x + 3.5f,
                centre.y
            );
        else
            disclosure.addTriangle(
                centre.x - 4.0f,
                centre.y - 2.5f,
                centre.x + 4.0f,
                centre.y - 2.5f,
                centre.x,
                centre.y + 3.5f
            );
        graphics.fillPath(disclosure);
    }

    void mouseDown(const MouseEvent& event) override
    {
        auto members = getMembers();
        bool allSelected =
            std::all_of(members.begin(), members.end(), [this](NodeID id) { return panel.selection.isSelected(id); });
        if (event.mods.isPopupMenu())
        {
            if (!allSelected)
                panel.selectOnly(members);
            panel.showSelectionMenu(event.getScreenPosition());
            return;
        }
        selectOnlyOnMouseUp = allSelected && !event.mods.isCommandDown() && !event.mods.isShiftDown();
        if (event.mods.isCommandDown() || event.mods.isShiftDown())
        {
            for (auto id : members)
                if (event.mods.isCommandDown() && allSelected)
                    panel.selection.deselect(id);
                else
                    panel.selection.addToSelection(id);
        }
        else if (!allSelected)
            panel.selectOnly(members);
        panel.beginDraggingNodes(panel.getSelectedNodes(), event);
    }

    void mouseDrag(const MouseEvent& event) override
    {
        auto members = getMembers();
        if (!event.mods.isPopupMenu()
            && std::all_of(members.begin(), members.end(), [this](NodeID id) { return panel.selection.isSelected(id); }))
            panel.dragNodes(event);
    }

    void mouseUp(const MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            return;
        bool dragged = event.mouseWasDraggedSinceMouseDown();
        panel.endDraggingNodes(dragged);
        if (!dragged && selectOnlyOnMouseUp)
            panel.selectOnly(getMembers());
        if (!dragged && event.y < titleHeight)
        {
            if (event.x >= getWidth() - disclosureWidth)
            {
                if (auto* group = panel.graph.getGroups().find(groupId))
                    panel.graph.setGroupCollapsed(groupId, !group->collapsed);
            }
            else if (event.getNumberOfClicks() == 2)
                panel.showRenameGroupWindow(groupId);
        }
    }

    enum
    {
        titleHeight = 20,
        disclosureWidth = 22
    };

    GraphEditorPanel& panel;
    uint32 groupId;
    String name;
    bool bypassed = false, collapsed = false, selectOnlyOnMouseUp = false;
};

GraphEditorPanel::GraphEditorPanel(PluginGraph& g)
    : graph(g)
{
    graph.addChangeListener(this);
    graph.getUndoManager().addChangeListener(this);
    selection.addChangeListener(this);
    auto accent = findColour(TextEditor::focusedOutlineColourId);
    lasso.setColour(LassoComponent<NodeID>::lassoFillColourId, accent.withAlpha(0.15f));
    lasso.setColour(LassoComponent<NodeID>::lassoOutlineColourId, accent.withAlpha(0.8f));
    lasso.setAlwaysOnTop(true);
    addChildComponent(lasso);
    setOpaque(true);
    setWantsKeyboardFocus(true);
    documentGeneration = graph.getDocumentGeneration();
    setSize(GraphViewGeometry::canvasWidth, GraphViewGeometry::canvasHeight);
    if (graph.getNativeEditorHost() == nullptr)
    {
        localCommandManager.registerAllCommandsForTarget(this);
        localCommandManager.setFirstCommandTarget(this);
        addKeyListener(localCommandManager.getKeyMappings());
    }
}

ApplicationCommandManager& GraphEditorPanel::getCommandManager()
{
    auto* host = graph.getNativeEditorHost();
    return host != nullptr ? host->getCommandManager() : localCommandManager;
}

GraphEditorPanel::~GraphEditorPanel()
{
    cancelGestures(documentGeneration != graph.getDocumentGeneration());
    fileChooser.reset();
    renameWindow.reset();
    selection.removeChangeListener(this);
    graph.getUndoManager().removeChangeListener(this);
    graph.removeChangeListener(this);
    draggingConnector = nullptr;
    groupFrames.clear();
    nodes.clear();
    connectors.clear();
}

void GraphEditorPanel::paint(Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));
}

void GraphEditorPanel::mouseDown(const MouseEvent& e)
{
    grabKeyboardFocus();
    cancelGestures(documentGeneration != graph.getDocumentGeneration());
    bool pan = graphViewport != nullptr && !e.source.isTouch() && e.mods.isRightButtonDown();
    if (pan)
    {
        backgroundGesture = BackgroundGesture::pan;
        graphViewport->beginPan(e);
        return;
    }
    if (!e.mods.isCommandDown() && !e.mods.isShiftDown())
        selection.deselectAll();
    if (e.source.isTouch())
    {
        originalTouchPos = e.position.toInt();
        startTimer(750);
    }
    else if (e.mods.isPopupMenu())
        showPopupMenu(e.position.toInt());
    if (!e.mods.isPopupMenu() && !e.mods.isShiftDown())
    {
        backgroundGesture = BackgroundGesture::lasso;
        lasso.beginLasso(e, this);
        lasso.toFront(false);
    }
}

void GraphEditorPanel::mouseUp(const MouseEvent& e)
{
    bool showMenu = backgroundGesture == BackgroundGesture::pan && !e.mouseWasDraggedSinceMouseDown();
    if (backgroundGesture == BackgroundGesture::pan && graphViewport != nullptr)
        graphViewport->endPan();
    backgroundGesture = BackgroundGesture::none;
    stopTimer();
    lasso.endLasso();
    if (showMenu)
        showPopupMenu(e.position.toInt());
}

void GraphEditorPanel::mouseDrag(const MouseEvent& e)
{
    if (backgroundGesture == BackgroundGesture::pan)
    {
        if (graphViewport != nullptr && e.mouseWasDraggedSinceMouseDown())
            graphViewport->dragPan(e);
        return;
    }
    if (e.source.isTouch() && e.getDistanceFromDragStart() > 5)
        stopTimer();
    if (backgroundGesture == BackgroundGesture::lasso && !e.mods.isShiftDown())
        lasso.dragLasso(e);
}

void GraphEditorPanel::createNewPlugin(const PluginDescriptionAndPreference& desc, Point<int> position)
{
    auto normalised = toNormalised(position);
    graph.addPlugin(
        desc,
        normalised,
        [safe = SafePointer<GraphEditorPanel>(this),
         generation = graph.getDocumentGeneration()](const Result& result, PluginGraph::NodeID)
        {
            if (safe != nullptr && safe->graph.getDocumentGeneration() == generation)
                safe->showError("Couldn't create plugin", result);
        }
    );
}

GraphEditorPanel::PluginComponent* GraphEditorPanel::getComponentForPlugin(AudioProcessorGraphMT::NodeID nodeID) const
{
    for (auto* fc : nodes)
        if (fc->pluginID == nodeID)
            return fc;

    return nullptr;
}

GraphEditorPanel::ConnectorComponent*
GraphEditorPanel::getComponentForConnection(const AudioProcessorGraphMT::Connection& conn) const
{
    for (auto* cc : connectors)
        if (cc->connection == conn)
            return cc;

    return nullptr;
}

GraphEditorPanel::PinComponent* GraphEditorPanel::findPinAt(Point<float> pos) const
{
    for (auto* fc : nodes)
    {
        if (!fc->isVisible())
            continue;
        // NB: A Visual Studio optimiser error means we have to put this Component* in a local
        // variable before trying to cast it, or it gets mysteriously optimised away..
        auto* comp = fc->getComponentAt(pos.toInt() - fc->getPosition());

        if (auto* pin = dynamic_cast<PinComponent*>(comp))
            return pin;
    }

    return nullptr;
}

void GraphEditorPanel::changeListenerCallback(ChangeBroadcaster* source)
{
    if (source == &selection || source == &graph.getUndoManager())
    {
        repaint();
        for (auto* node : nodes)
            node->repaint();
        for (auto* frame : groupFrames)
            frame->repaint();
        getCommandManager().commandStatusChanged();
        return;
    }
    bool replaced = documentGeneration != graph.getDocumentGeneration();
    if (replaced)
    {
        cancelGestures(true);
        fileChooser.reset();
        renameWindow.reset();
        menu.reset();
        selection.deselectAll();
        nodes.clear();
        connectors.clear();
        groupFrames.clear();
    }
    documentGeneration = graph.getDocumentGeneration();
    updateComponents();
    if (replaced && graphViewport != nullptr)
        graphViewport->resetView();
    getCommandManager().commandStatusChanged();
}

void GraphEditorPanel::updateComponents()
{
    if (documentGeneration != graph.getDocumentGeneration())
    {
        changeListenerCallback(&graph);
        return;
    }
    auto previousCentre =
        graphViewport != nullptr
            ? graphViewport->getLogicalCentre()
            : Point<double>(GraphViewGeometry::canvasWidth * 0.5, GraphViewGeometry::canvasHeight * 0.5);
    for (int i = nodes.size(); --i >= 0;)
        if (!graph.isNodeVisible(nodes.getUnchecked(i)->pluginID)
            || graph.graph.getNodeForId(nodes.getUnchecked(i)->pluginID) != nodes.getUnchecked(i)->processorNode.get())
        {
            cancelGestures(false);
            selection.deselect(nodes.getUnchecked(i)->pluginID);
            nodes.remove(i);
        }

    for (int i = connectors.size(); --i >= 0;)
        if (!graph.graph.isConnected(connectors.getUnchecked(i)->connection)
            || !graph.isNodeVisible(connectors.getUnchecked(i)->connection.source.nodeID)
            || !graph.isNodeVisible(connectors.getUnchecked(i)->connection.destination.nodeID))
            connectors.remove(i);

    for (auto* fc : nodes)
        fc->update();

    for (auto* f : graph.graph.getNodes())
    {
        if (graph.isNodeVisible(f->nodeID) && getComponentForPlugin(f->nodeID) == nullptr)
        {
            auto* comp = nodes.add(new PluginComponent(*this, f->nodeID));
            addAndMakeVisible(comp);
            comp->update();
        }
    }

    auto& groups = graph.getGroups();
    for (int i = groupFrames.size(); --i >= 0;)
        if (groups.find(groupFrames.getUnchecked(i)->groupId) == nullptr)
            groupFrames.remove(i);
    for (auto& group : groups.getAll())
        if (getComponentForGroup(group.id) == nullptr)
            addAndMakeVisible(groupFrames.add(new GroupComponent(*this, group.id)));

    std::function<void(const NodeGroup&)> updateTree = [&](const NodeGroup& group)
    {
        for (auto& child : group.childGroups)
            if (auto* childGroup = groups.find(child.groupId))
                updateTree(*childGroup);
        auto* frame = getComponentForGroup(group.id);
        frame->update(group);
        frame->toBack();
    };
    for (auto& group : groups.getAll())
        if (groups.findParentOf(group.id) == nullptr)
            updateTree(group);
    for (auto* node : nodes)
        node->setVisible(getCollapsedGroupFor(node->pluginID) == nullptr);

    for (auto& c : graph.graph.getConnections())
    {
        if (graph.isNodeVisible(c.source.nodeID)
            && graph.isNodeVisible(c.destination.nodeID)
            && getComponentForConnection(c) == nullptr)
        {
            auto* comp = connectors.add(new ConnectorComponent(*this));
            addAndMakeVisible(comp);

            comp->setInput(c.source);
            comp->setOutput(c.destination);
        }
    }
    for (auto* connector : connectors)
    {
        connector->toBack();
        auto* sourceGroup = getCollapsedGroupFor(connector->connection.source.nodeID);
        auto* destinationGroup = getCollapsedGroupFor(connector->connection.destination.nodeID);
        connector->setVisible(sourceGroup == nullptr || sourceGroup != destinationGroup);
        connector->update();
    }
    for (auto* frame : groupFrames)
    {
        auto* ancestor = groups.findParentOf(frame->groupId);
        bool hidden = false;
        while (ancestor != nullptr)
        {
            hidden = hidden || ancestor->collapsed;
            ancestor = groups.findParentOf(ancestor->id);
        }
        frame->setVisible(!hidden);
    }
    updateLayoutBounds();
    if (graphViewport != nullptr)
        graphViewport->contentChanged(previousCentre);
}

void GraphEditorPanel::showPopupMenu(Point<int> mousePos)
{
    menu.reset(new PopupMenu);

    if (auto* host = graph.getNativeEditorHost())
    {
        host->addPluginsToMenu(*menu);
        menu->addSeparator();
        addSelectionItemsToMenu(*menu, mousePos);

        menu->showMenuAsync(
            {},
            ModalCallbackFunction::create(
                [safe = SafePointer<GraphEditorPanel>(this), generation = graph.getDocumentGeneration(), mousePos](int r)
                {
                    if (safe == nullptr || safe->graph.getDocumentGeneration() != generation)
                        return;
                    if (auto* currentHost = safe->graph.getNativeEditorHost())
                        currentHost->createPluginFromMenu(r, safe->toNormalised(mousePos));
                }
            )
        );
    }
}

void GraphEditorPanel::beginConnectorDrag(
    AudioProcessorGraphMT::NodeAndChannel source,
    AudioProcessorGraphMT::NodeAndChannel dest,
    const MouseEvent& e
)
{
    if (documentGeneration != graph.getDocumentGeneration())
    {
        cancelGestures(true);
        return;
    }
    grabKeyboardFocus();
    auto* c = dynamic_cast<ConnectorComponent*>(e.originalComponent);
    connectors.removeObject(c, false);
    draggingConnector.reset(c);

    if (connectorEdit == nullptr)
        connectorEdit = std::make_unique<PluginGraph::Edit>(graph, "Connect");

    if (draggingConnector == nullptr)
        draggingConnector.reset(new ConnectorComponent(*this));

    draggingConnector->setInput(source);
    draggingConnector->setOutput(dest);

    addAndMakeVisible(draggingConnector.get());
    draggingConnector->toFront(false);

    dragConnector(e);
}

void GraphEditorPanel::dragConnector(const MouseEvent& e)
{
    if (documentGeneration != graph.getDocumentGeneration())
    {
        cancelGestures(true);
        return;
    }
    auto e2 = e.getEventRelativeTo(this);

    if (draggingConnector != nullptr)
    {
        draggingConnector->setTooltip({});

        auto pos = e2.position;

        if (auto* pin = findPinAt(pos))
        {
            auto connection = draggingConnector->connection;

            if (connection.source.nodeID == AudioProcessorGraphMT::NodeID() && !pin->isInput)
                connection.source = pin->pin;
            else if (connection.destination.nodeID == AudioProcessorGraphMT::NodeID() && pin->isInput)
                connection.destination = pin->pin;

            if (graph.graph.canConnect(connection))
            {
                pos = (pin->getParentComponent()->getPosition() + pin->getBounds().getCentre()).toFloat();
                draggingConnector->setTooltip(pin->getTooltip());
            }
        }

        if (draggingConnector->connection.source.nodeID == AudioProcessorGraphMT::NodeID())
            draggingConnector->dragStart(pos);
        else
            draggingConnector->dragEnd(pos);
    }
}

void GraphEditorPanel::endDraggingConnector(const MouseEvent& e)
{
    if (draggingConnector == nullptr)
    {
        cancelGestures(documentGeneration != graph.getDocumentGeneration());
        return;
    }

    draggingConnector->setTooltip({});

    auto e2 = e.getEventRelativeTo(this);
    auto connection = draggingConnector->connection;

    draggingConnector = nullptr;

    bool connected = false;
    auto* pin = findPinAt(e2.position);
    if (pin != nullptr)
    {
        if (connection.source.nodeID == AudioProcessorGraphMT::NodeID())
        {
            if (!pin->isInput)
                connection.source = pin->pin;
        }
        else
        {
            if (pin->isInput)
                connection.destination = pin->pin;
        }
        if (documentGeneration == graph.getDocumentGeneration() && graph.graph.canConnect(connection))
            connected = graph.addConnection(connection).wasOk();
    }
    if (!connected && pin != nullptr && originalConnection && documentGeneration == graph.getDocumentGeneration())
        showError("Restore connection", graph.restoreConnection(*originalConnection));
    originalConnection.reset();
    connectorEdit.reset();
    updateComponents();
}

void GraphEditorPanel::timerCallback()
{
    stopTimer();
    showPopupMenu(originalTouchPos);
}

void GraphEditorPanel::showSelectionMenu(Point<int> screenPosition)
{
    menu = std::make_unique<PopupMenu>();
    addSelectionItemsToMenu(*menu, getLocalPoint(nullptr, screenPosition));
    menu->showMenuAsync(PopupMenu::Options{}.withTargetScreenArea(juce::Rectangle<int>{}.withPosition(screenPosition)));
}

void GraphEditorPanel::addSelectionItemsToMenu(PopupMenu& items, std::optional<Point<int>> insertPosition)
{
    auto action =
        [safe = SafePointer<GraphEditorPanel>(this), generation = graph.getDocumentGeneration()](auto callback)
    {
        return [safe, generation, callback]
        {
            if (safe != nullptr && safe->graph.getDocumentGeneration() == generation)
                callback(*safe);
        };
    };
    if (auto selected = getSingleSelectedNode())
    {
        if (auto* node = graph.graph.getNodeForId(*selected); node != nullptr && !graph.isParentNode(*selected))
        {
            auto id = *selected;
            auto* processor = node->getProcessor();
            bool available = dynamic_cast<MissingPlugin*>(processor) == nullptr;
            if (processor->hasEditor())
                items.addItem(
                    "Show plugin GUI",
                    action([id](GraphEditorPanel& panel) { panel.showNodeWindow(id, PluginWindow::Type::normal); })
                );
            items.addItem(
                "Show all programs",
                available,
                false,
                action([id](GraphEditorPanel& panel) { panel.showNodeWindow(id, PluginWindow::Type::programs); })
            );
            items.addItem(
                "Show all parameters",
                available,
                false,
                action([id](GraphEditorPanel& panel) { panel.showNodeWindow(id, PluginWindow::Type::generic); })
            );
            items.addItem(
                "Show debug log",
                available,
                false,
                action([id](GraphEditorPanel& panel) { panel.showNodeWindow(id, PluginWindow::Type::debug); })
            );
#if JUCE_PLUGINHOST_ARA && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)
            if (auto* instance = dynamic_cast<AudioPluginInstance*>(processor))
                if (available && instance->getPluginDescription().hasARAExtension && (bool)node->properties["useARA"])
                    items.addItem(
                        "Show ARA host controls",
                        action([id](GraphEditorPanel& panel) { panel.showNodeWindow(id, PluginWindow::Type::araHost); })
                    );
#endif
            items.addItem(
                "Configure Audio I/O",
                available,
                false,
                action([id](GraphEditorPanel& panel) { panel.showNodeWindow(id, PluginWindow::Type::audioIO); })
            );
            items.addItem(
                "Test state save/load",
                available,
                false,
                action(
                    [id](GraphEditorPanel& panel)
                    {
                        if (auto* current = panel.graph.graph.getNodeForId(id))
                        {
                            MemoryBlock state;
                            current->getProcessor()->getStateInformation(state);
                            current->getProcessor()->setStateInformation(state.getData(), (int)state.getSize());
                            panel.graph.setChangedFlag(true);
                        }
                    }
                )
            );
            items.addItem("Rename Node...", action([id](GraphEditorPanel& panel) { panel.showRenameNodeWindow(id); }));
            items.addSeparator();
        }
    }
    auto* manager = &getCommandManager();
    items.addCommandItem(manager, StandardApplicationCommandIDs::undo);
    items.addCommandItem(manager, StandardApplicationCommandIDs::redo);
    items.addSeparator();
    items.addCommandItem(manager, StandardApplicationCommandIDs::cut);
    items.addCommandItem(manager, StandardApplicationCommandIDs::copy);
    items.addCommandItem(manager, StandardApplicationCommandIDs::paste);
    items.addCommandItem(manager, CommandIDs::duplicateSelection);
    items.addCommandItem(manager, CommandIDs::deleteSelection);
    items.addCommandItem(manager, StandardApplicationCommandIDs::selectAll);
    items.addSeparator();
    items.addCommandItem(manager, CommandIDs::disconnectSelection);
    items.addCommandItem(manager, CommandIDs::toggleBypassSelection);
    items.addSeparator();
    items.addCommandItem(manager, CommandIDs::groupSelection);
    items.addCommandItem(manager, CommandIDs::ungroupSelection);
    items.addCommandItem(manager, CommandIDs::renameGroup);
#if !JUCE_IOS && !JUCE_ANDROID
    items.addSeparator();
    items.addCommandItem(manager, CommandIDs::saveNodeState);
    items.addCommandItem(manager, CommandIDs::loadNodeState);
    items.addCommandItem(manager, CommandIDs::saveGroup);
    items.addCommandItem(manager, CommandIDs::loadGroupState);
    auto position = insertPosition.value_or(visibleCentre());
    items.addItem(
        "Insert group from file...",
        action(
            [position](GraphEditorPanel& panel)
            {
                panel.chooseFile(
                    "Insert group from file",
                    PluginGraph::nodeGroupWildcard,
                    false,
                    [&panel, position](const File& file)
                    { return panel.graph.insertGroupFromFile(file, panel.toNormalised(position)); }
                );
            }
        )
    );
#endif
}

ApplicationCommandTarget* GraphEditorPanel::getNextCommandTarget()
{
    return nullptr;
}

void GraphEditorPanel::getAllCommands(Array<CommandID>& commands)
{
    const CommandID ids[] = {
        StandardApplicationCommandIDs::undo,
        StandardApplicationCommandIDs::redo,
        StandardApplicationCommandIDs::cut,
        StandardApplicationCommandIDs::copy,
        StandardApplicationCommandIDs::paste,
        StandardApplicationCommandIDs::selectAll,
        CommandIDs::deleteSelection,
        CommandIDs::disconnectSelection,
        CommandIDs::toggleBypassSelection,
        CommandIDs::groupSelection,
        CommandIDs::ungroupSelection,
        CommandIDs::renameGroup,
        CommandIDs::saveNodeState,
        CommandIDs::loadNodeState,
        CommandIDs::saveGroup,
        CommandIDs::loadGroupState,
        CommandIDs::duplicateSelection,
        CommandIDs::zoomIn,
        CommandIDs::zoomOut,
        CommandIDs::resetZoom,
        CommandIDs::zoomToFit
    };
    commands.addArray(ids, numElementsInArray(ids));
}

void GraphEditorPanel::getCommandInfo(CommandID id, ApplicationCommandInfo& info)
{
    auto selected = getSelectedNodes();
    bool hasSelection = !selected.isEmpty();
    bool single = selected.size() == 1;
    bool groupSelected = getSelectedGroupId() != 0;
    bool copyable = false, grouped = false;
    bool editable = false;
    std::vector<uint32> groupable;
    for (auto nodeId : selected)
    {
        if (auto* node = graph.graph.getNodeForId(nodeId))
        {
            editable = editable || !graph.isParentNode(nodeId);
            if (PluginGraph::isGroupable(*node))
            {
                copyable = true;
                groupable.push_back(nodeId.uid);
            }
            grouped = grouped || graph.getGroups().findGroupOf(nodeId.uid) != nullptr;
        }
    }
    auto& history = graph.getUndoManager();
    switch (id)
    {
    case StandardApplicationCommandIDs::undo:
        info.setInfo(
            history.canUndo() ? "Undo " + history.getUndoDescription() : "Undo",
            "Undoes the last graph edit",
            "Edit",
            0
        );
        info.addDefaultKeypress('z', ModifierKeys::commandModifier);
        info.setActive(history.canUndo());
        break;
    case StandardApplicationCommandIDs::redo:
        info.setInfo(
            history.canRedo() ? "Redo " + history.getRedoDescription() : "Redo",
            "Redoes the last graph edit",
            "Edit",
            0
        );
        info.addDefaultKeypress('z', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
#if !JUCE_MAC
        info.addDefaultKeypress('y', ModifierKeys::commandModifier);
#endif
        info.setActive(history.canRedo());
        break;
    case StandardApplicationCommandIDs::cut:
        info.setInfo("Cut", "Copies and deletes the selected nodes", "Edit", 0);
        info.addDefaultKeypress('x', ModifierKeys::commandModifier);
        info.setActive(copyable);
        break;
    case StandardApplicationCommandIDs::copy:
        info.setInfo("Copy", "Copies the selected nodes", "Edit", 0);
        info.addDefaultKeypress('c', ModifierKeys::commandModifier);
        info.setActive(copyable);
        break;
    case StandardApplicationCommandIDs::paste:
        info.setInfo("Paste", "Pastes copied nodes", "Edit", 0);
        info.addDefaultKeypress('v', ModifierKeys::commandModifier);
        break;
    case StandardApplicationCommandIDs::selectAll:
        info.setInfo("Select All", "Selects all nodes", "Edit", 0);
        info.addDefaultKeypress('a', ModifierKeys::commandModifier);
        info.setActive(!nodes.isEmpty());
        break;
    case CommandIDs::duplicateSelection:
        info.setInfo("Duplicate", "Duplicates the selected nodes", "Edit", 0);
        info.addDefaultKeypress('d', ModifierKeys::commandModifier);
        info.setActive(copyable);
        break;
    case CommandIDs::deleteSelection:
        info.setInfo("Delete", "Deletes the selected nodes", "Edit", 0);
        info.addDefaultKeypress(KeyPress::deleteKey, ModifierKeys::noModifiers);
        info.addDefaultKeypress(KeyPress::backspaceKey, ModifierKeys::noModifiers);
        info.setActive(editable);
        break;
    case CommandIDs::disconnectSelection:
        info.setInfo("Disconnect All Pins", "Removes connections of the selected nodes", "Edit", 0);
        info.setActive(hasSelection);
        break;
    case CommandIDs::toggleBypassSelection:
        info.setInfo(
            groupSelected ? "Toggle Group Bypass" : "Toggle Bypass",
            "Toggles bypass of the selection",
            "Edit",
            0
        );
        info.addDefaultKeypress('b', ModifierKeys::commandModifier);
        info.setActive(editable);
        break;
    case CommandIDs::groupSelection:
    {
        info.setInfo("Group", "Groups the selected nodes", "Edit", 0);
        info.addDefaultKeypress('g', ModifierKeys::commandModifier);
        auto groups = graph.getGroups().getGroupIdsWithin(groupable);
        auto units = groupable.size() + groups.size();
        for (auto groupId : groups)
            units -= graph.getGroups().getNodeUids(groupId).size();
        info.setActive(units >= 2);
        break;
    }
    case CommandIDs::ungroupSelection:
        info.setInfo("Ungroup", "Ungroups the selected groups", "Edit", 0);
        info.addDefaultKeypress('g', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
        info.setActive(grouped);
        break;
    case CommandIDs::renameGroup:
        info.setInfo("Rename Group...", "Renames the selected group", "Edit", 0);
        info.addDefaultKeypress(KeyPress::F2Key, ModifierKeys::noModifiers);
        info.setActive(groupSelected);
        break;
    case CommandIDs::saveNodeState:
        info.setInfo("Save Node State...", "Saves the selected node state", "Edit", 0);
        info.setActive(single);
        break;
    case CommandIDs::loadNodeState:
        info.setInfo("Load Node State...", "Loads state onto the selected node", "Edit", 0);
        info.setActive(single);
        break;
    case CommandIDs::saveGroup:
        info.setInfo("Save Group...", "Exports the selected group", "Edit", 0);
        info.setActive(groupSelected);
        break;
    case CommandIDs::loadGroupState:
        info.setInfo("Load Group State...", "Loads state onto the selected group", "Edit", 0);
        info.setActive(groupSelected);
        break;
    case CommandIDs::zoomIn:
        info.setInfo("Zoom In", "Magnifies the graph canvas", "View", 0);
        info.addDefaultKeypress('=', ModifierKeys::commandModifier);
        info.addDefaultKeypress('+', ModifierKeys::commandModifier);
        info.addDefaultKeypress('=', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
        info.addDefaultKeypress('+', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
        info.setActive(graphViewport != nullptr && graphViewport->getZoom() < GraphViewGeometry::maximumZoom);
        break;
    case CommandIDs::zoomOut:
        info.setInfo("Zoom Out", "Reduces the graph magnification", "View", 0);
        info.addDefaultKeypress('-', ModifierKeys::commandModifier);
        info.setActive(graphViewport != nullptr && graphViewport->getZoom() > GraphViewGeometry::minimumZoom);
        break;
    case CommandIDs::resetZoom:
        info.setInfo("Reset Zoom", "Shows the graph at 100%", "View", 0);
        info.addDefaultKeypress('0', ModifierKeys::commandModifier);
        info.setActive(graphViewport != nullptr);
        break;
    case CommandIDs::zoomToFit:
        info.setInfo("Zoom to Fit", "Fits all graph nodes and groups", "View", 0);
        info.addDefaultKeypress('f', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
        info.setActive(graphViewport != nullptr);
        break;
    default:
        break;
    }
    if (graph.isEditInProgress() || documentGeneration != graph.getDocumentGeneration())
        info.setActive(false);
}

bool GraphEditorPanel::perform(const InvocationInfo& invocation)
{
    if (documentGeneration != graph.getDocumentGeneration())
    {
        changeListenerCallback(&graph);
        return false;
    }
    if (invocation.invocationMethod == InvocationInfo::fromKeyPress)
    {
        auto* origin = invocation.originatingComponent;
        if (origin != nullptr && origin->getTopLevelComponent() != getTopLevelComponent())
            return false;
        auto* focus = Component::getCurrentlyFocusedComponent();
        if (focus != nullptr
            && (focus->getTopLevelComponent() != getTopLevelComponent()
                || dynamic_cast<TextEditor*>(focus) != nullptr
                || focus->findParentComponentOfClass<TextEditor>() != nullptr))
            return false;
    }
    if (graph.isEditInProgress())
        return false;
    switch (invocation.commandID)
    {
    case StandardApplicationCommandIDs::undo:
        graph.getUndoManager().undo();
        break;
    case StandardApplicationCommandIDs::redo:
        graph.getUndoManager().redo();
        break;
    case StandardApplicationCommandIDs::cut:
    {
        PluginGraph::Edit edit(graph, "Cut");
        copySelection();
        deleteSelection();
        break;
    }
    case StandardApplicationCommandIDs::copy:
        copySelection();
        break;
    case StandardApplicationCommandIDs::paste:
        pasteFromClipboard();
        break;
    case StandardApplicationCommandIDs::selectAll:
    {
        Array<NodeID> all;
        for (auto* node : graph.graph.getNodes())
            all.add(node->nodeID);
        selectOnly(all);
        break;
    }
    case CommandIDs::duplicateSelection:
        duplicateSelection();
        break;
    case CommandIDs::deleteSelection:
        deleteSelection();
        break;
    case CommandIDs::disconnectSelection:
    {
        PluginGraph::Edit edit(graph, "Disconnect");
        for (auto id : getSelectedNodes())
            graph.disconnectNode(id);
        break;
    }
    case CommandIDs::toggleBypassSelection:
        toggleBypassOfSelection();
        break;
    case CommandIDs::groupSelection:
        if (auto groupId = graph.groupNodes(getSelectedNodes()))
        {
            Array<NodeID> members;
            for (auto uid : graph.getGroups().getNodeUids(groupId))
                members.add(NodeID(uid));
            selectOnly(members);
        }
        break;
    case CommandIDs::ungroupSelection:
    {
        PluginGraph::Edit edit(graph, "Ungroup");
        std::set<uint32> groups;
        if (auto id = getSelectedGroupId())
            groups.insert(id);
        else
            for (auto nodeId : getSelectedNodes())
                if (auto* group = graph.getGroups().findGroupOf(nodeId.uid))
                    groups.insert(group->id);
        for (auto id : groups)
            graph.ungroup(id);
        break;
    }
    case CommandIDs::renameGroup:
        showRenameGroupWindow(getSelectedGroupId());
        break;
    case CommandIDs::saveNodeState:
        if (auto id = getSingleSelectedNode())
            chooseFile(
                "Save node state",
                PluginGraph::nodeStateWildcard,
                true,
                [this, id = *id](const File& file) { return graph.saveNodeState(id, file); }
            );
        break;
    case CommandIDs::loadNodeState:
        if (auto id = getSingleSelectedNode())
            chooseFile(
                "Load node state",
                PluginGraph::nodeStateWildcard,
                false,
                [this, id = *id](const File& file) { return graph.loadNodeState(id, file); }
            );
        break;
    case CommandIDs::saveGroup:
        if (auto id = getSelectedGroupId())
            chooseFile(
                "Save group",
                PluginGraph::nodeGroupWildcard,
                true,
                [this, id](const File& file) { return graph.saveGroup(id, file); }
            );
        break;
    case CommandIDs::loadGroupState:
        if (auto id = getSelectedGroupId())
            chooseFile(
                "Load group state",
                PluginGraph::nodeGroupWildcard,
                false,
                [this, id](const File& file) { return graph.loadGroupState(id, file); }
            );
        break;
    case CommandIDs::zoomIn:
    case CommandIDs::zoomOut:
    case CommandIDs::resetZoom:
    case CommandIDs::zoomToFit:
        if (graphViewport == nullptr)
            return false;
        if (invocation.commandID == CommandIDs::zoomToFit)
            graphViewport->zoomToFit();
        else if (invocation.commandID == CommandIDs::resetZoom)
            graphViewport->setZoom(1.0);
        else
            graphViewport->setZoom(
                graphViewport->getZoom()
                * (invocation.commandID == CommandIDs::zoomIn ? GraphViewGeometry::zoomStep
                                                              : 1.0 / GraphViewGeometry::zoomStep)
            );
        break;
    default:
        return false;
    }
    updateComponents();
    getCommandManager().commandStatusChanged();
    return true;
}

void GraphEditorPanel::mouseWheelMove(const MouseEvent& event, const MouseWheelDetails& wheel)
{
    if (graphViewport != nullptr)
        graphViewport->wheelMove(event, wheel);
    else
        Component::mouseWheelMove(event, wheel);
}

bool GraphEditorPanel::keyPressed(const KeyPress& key)
{
    if (key == KeyPress::escapeKey)
    {
        cancelGestures(documentGeneration != graph.getDocumentGeneration());
        updateComponents();
        return true;
    }
    return false;
}

void GraphEditorPanel::cancelGestures(bool documentReplaced)
{
    stopTimer();
    lasso.endLasso();
    if (graphViewport != nullptr)
        graphViewport->endPan();
    backgroundGesture = BackgroundGesture::none;
    for (auto* node : nodes)
        node->stopTimer();
    draggingConnector.reset();
    if (!documentReplaced)
    {
        if (originalConnection)
            showError("Restore connection", graph.restoreConnection(*originalConnection));
        for (auto& entry : dragStartPositions)
            graph.setNodePosition(entry.first, entry.second);
    }
    originalConnection.reset();
    dragStartPositions.clear();
    connectorEdit.reset();
    dragEdit.reset();
}

void GraphEditorPanel::findLassoItemsInArea(Array<NodeID>& found, const juce::Rectangle<int>& area)
{
    for (auto* node : nodes)
        if (node->isVisible() && node->getBounds().intersects(area))
            found.addIfNotAlreadyThere(node->pluginID);
    for (auto* frame : groupFrames)
        if (frame->isVisible() && frame->collapsed && frame->getBounds().intersects(area))
            for (auto id : frame->getMembers())
                found.addIfNotAlreadyThere(id);
}

SelectedItemSet<GraphEditorPanel::NodeID>& GraphEditorPanel::getLassoSelection()
{
    return selection;
}

Array<GraphEditorPanel::NodeID> GraphEditorPanel::getSelectedNodes() const
{
    return selection.getItemArray();
}

std::optional<GraphEditorPanel::NodeID> GraphEditorPanel::getSingleSelectedNode() const
{
    if (selection.getNumSelected() == 1)
        return selection.getSelectedItem(0);
    return {};
}

uint32 GraphEditorPanel::getSelectedGroupId() const
{
    auto& selected = selection.getItemArray();
    if (selected.isEmpty())
        return 0;
    for (auto& group : graph.getGroups().getAll())
    {
        auto members = graph.getGroups().getNodeUids(group.id);
        if (members.size() == static_cast<size_t>(selected.size())
            && std::all_of(
                members.begin(),
                members.end(),
                [&selected](uint32 uid) { return selected.contains(NodeID(uid)); }
            ))
            return group.id;
    }
    return 0;
}

void GraphEditorPanel::selectOnly(const Array<NodeID>& ids)
{
    selection.deselectAll();
    for (auto id : ids)
        if (graph.isNodeVisible(id))
            selection.addToSelection(id);
}

void GraphEditorPanel::beginDraggingNodes(const Array<NodeID>& ids, const MouseEvent& event)
{
    cancelGestures(documentGeneration != graph.getDocumentGeneration());
    if (documentGeneration != graph.getDocumentGeneration())
        return;
    grabKeyboardFocus();
    nodeDragStartScreenPosition = event.eventComponent->localPointToGlobal(event.position);
    for (auto id : ids)
        dragStartPositions[id] = graph.getNodePosition(id);
}

void GraphEditorPanel::dragNodes(const MouseEvent& event)
{
    if (documentGeneration != graph.getDocumentGeneration())
    {
        cancelGestures(true);
        return;
    }
    if (dragStartPositions.empty() || !event.mouseWasDraggedSinceMouseDown())
        return;
    if (dragEdit == nullptr)
        dragEdit = std::make_unique<PluginGraph::Edit>(graph, "Move");
    auto offset = event.eventComponent->localPointToGlobal(event.position) - nodeDragStartScreenPosition;
    double zoom = graphViewport != nullptr ? graphViewport->getZoom() : 1.0;
    auto normalisedOffset = GraphViewGeometry::normalisePosition(offset.toDouble() / zoom);
    for (auto& entry : dragStartPositions)
        graph.setNodePosition(entry.first, entry.second + normalisedOffset);
    updateComponents();
}

void GraphEditorPanel::endDraggingNodes(bool wasDragged)
{
    if (documentGeneration != graph.getDocumentGeneration())
    {
        cancelGestures(true);
        return;
    }
    if (!wasDragged)
    {
        cancelGestures(false);
        return;
    }
    dragStartPositions.clear();
    dragEdit.reset();
}

Point<double> GraphEditorPanel::toNormalised(Point<int> position) const
{
    return GraphViewGeometry::normalisePosition((position - layoutOrigin).toDouble());
}

Point<double> GraphEditorPanel::getPasteOffset() const
{
    return GraphViewGeometry::normalisePosition({20, 20});
}

Point<int> GraphEditorPanel::visibleCentre() const
{
    return graphViewport != nullptr ? graphViewport->getVisiblePanelCentre() : getLocalBounds().getCentre();
}

void GraphEditorPanel::copySelection()
{
    if (auto xml = graph.createSnippetXml(getSelectedNodes()))
        SystemClipboard::copyTextToClipboard(xml->toString());
}

void GraphEditorPanel::pasteFromClipboard()
{
    auto xml = parseXML(SystemClipboard::getTextFromClipboard());
    if (xml == nullptr || !PluginGraph::isSnippetXml(*xml))
        return;
    auto mouse = getMouseXYRelative();
    bool overGraph = graphViewport != nullptr ? juce::Rectangle<int>(
                                                    0,
                                                    0,
                                                    graphViewport->getViewport().getMaximumVisibleWidth(),
                                                    graphViewport->getViewport().getMaximumVisibleHeight()
                                                )
                                                    .contains(graphViewport->getViewport().getMouseXYRelative())
                                              : getLocalBounds().contains(mouse);
    auto position = overGraph
                      ? toNormalised(mouse)
                      : Point<double>(xml->getDoubleAttribute("x"), xml->getDoubleAttribute("y")) + getPasteOffset();
    insertSnippet(*xml, position);
}

void GraphEditorPanel::duplicateSelection()
{
    PluginGraph::Edit edit(graph, "Duplicate");
    if (auto xml = graph.createSnippetXml(getSelectedNodes()))
        insertSnippet(
            *xml,
            Point<double>(xml->getDoubleAttribute("x"), xml->getDoubleAttribute("y")) + getPasteOffset()
        );
}

void GraphEditorPanel::insertSnippet(const XmlElement& xml, Point<double> position)
{
    Array<NodeID> inserted;
    auto result = graph.insertSnippetXml(xml, position, &inserted);
    if (result.failed())
        showError("Paste", result);
    else
        selectOnly(inserted);
}

GraphEditorPanel::GroupComponent* GraphEditorPanel::getComponentForGroup(uint32 id) const
{
    for (auto* frame : groupFrames)
        if (frame->groupId == id)
            return frame;
    return nullptr;
}

GraphEditorPanel::GroupComponent* GraphEditorPanel::getCollapsedGroupFor(NodeID id) const
{
    auto* group = graph.getGroups().findGroupOf(id.uid);
    GroupComponent* collapsed = nullptr;
    while (group != nullptr)
    {
        if (group->collapsed)
            collapsed = getComponentForGroup(group->id);
        group = graph.getGroups().findParentOf(group->id);
    }
    return collapsed;
}

Point<float> GraphEditorPanel::getEndpointAnchor(NodeID id, int channel, bool output, Point<float> fallback) const
{
    if (auto* collapsed = getCollapsedGroupFor(id))
        return collapsed->getBounds().toFloat().getCentre();
    if (auto* component = getComponentForPlugin(id))
        return component->getPinPos(channel, !output);
    return fallback;
}

Point<float>
GraphEditorPanel::getVisibleEndpoint(NodeID id, int channel, bool output, Point<float> other, Point<float> fallback) const
{
    if (auto* collapsed = getCollapsedGroupFor(id))
    {
        auto bounds = collapsed->getBounds().toFloat();
        auto delta = other - bounds.getCentre();
        if (std::abs(delta.x) >= std::abs(delta.y))
            return {
                delta.x >= 0.0f ? bounds.getRight() : bounds.getX(),
                jlimit(bounds.getY() + 8.0f, bounds.getBottom() - 8.0f, other.y)
            };
        return {
            jlimit(bounds.getX() + 8.0f, bounds.getRight() - 8.0f, other.x),
            delta.y >= 0.0f ? bounds.getBottom() : bounds.getY()
        };
    }
    if (auto* component = getComponentForPlugin(id))
        return component->getPinPos(channel, !output);
    return fallback;
}

void GraphEditorPanel::updateLayoutBounds()
{
    graphVisualBounds = {};
    auto include = [this](juce::Rectangle<int> bounds)
    {
        bounds -= layoutOrigin;
        graphVisualBounds = graphVisualBounds.isEmpty() ? bounds : graphVisualBounds.getUnion(bounds);
    };
    for (auto* node : nodes)
        if (node->isVisible())
            include(node->getBounds());
    for (auto* frame : groupFrames)
        if (frame->isVisible())
            include(frame->getBounds());
    auto canvas = juce::Rectangle<int>(0, 0, GraphViewGeometry::canvasWidth, GraphViewGeometry::canvasHeight);
    auto bounds = canvas.getUnion(graphVisualBounds).expanded(GraphViewGeometry::padding);
    auto newOrigin = -bounds.getPosition();
    auto offset = newOrigin - layoutOrigin;
    layoutOrigin = newOrigin;
    for (auto* node : nodes)
        node->setTopLeftPosition(node->getPosition() + offset);
    for (auto* frame : groupFrames)
        frame->setTopLeftPosition(frame->getPosition() + offset);
    setSize(bounds.getWidth(), bounds.getHeight());
    for (auto* connector : connectors)
        connector->update();
}

void GraphEditorPanel::deleteSelection()
{
    PluginGraph::Edit edit(graph, "Delete");
    auto selected = getSelectedNodes();
    selection.deselectAll();
    for (auto id : selected)
        graph.removeNode(id);
}

void GraphEditorPanel::toggleBypassOfSelection()
{
    if (auto id = getSelectedGroupId())
    {
        graph.setGroupBypassed(id, !graph.getGroups().find(id)->bypassed);
        return;
    }
    PluginGraph::Edit edit(graph, "Bypass");
    auto selected = getSelectedNodes();
    bool anyActive = false;
    for (auto id : selected)
        if (auto* node = graph.graph.getNodeForId(id))
            anyActive = anyActive || !node->isBypassed();
    for (auto id : selected)
        graph.setNodeBypassed(id, anyActive);
}

void GraphEditorPanel::showRenameGroupWindow(uint32 id)
{
    auto* group = graph.getGroups().find(id);
    if (group == nullptr)
        return;
    renameWindow = std::make_unique<AlertWindow>("Rename group", String(), MessageBoxIconType::NoIcon, this);
    renameWindow->addTextEditor("name", group->name);
    renameWindow->addButton("OK", 1, KeyPress(KeyPress::returnKey));
    renameWindow->addButton("Cancel", 0, KeyPress(KeyPress::escapeKey));
    renameWindow->enterModalState(
        true,
        ModalCallbackFunction::create(
            [safe = SafePointer<GraphEditorPanel>(this),
             dialog = SafePointer<AlertWindow>(renameWindow.get()),
             generation = graph.getDocumentGeneration(),
             id](int result)
            {
                if (safe == nullptr
                    || dialog == nullptr
                    || safe->renameWindow.get() != dialog.getComponent()
                    || safe->graph.getDocumentGeneration() != generation)
                    return;
                auto name = safe->renameWindow->getTextEditorContents("name").trim();
                if (result == 1 && name.isNotEmpty())
                    safe->showError("Rename group", safe->graph.renameGroup(id, name));
                safe->renameWindow.reset();
            }
        ),
        false
    );
}

void GraphEditorPanel::showRenameNodeWindow(NodeID id)
{
    auto* node = graph.graph.getNodeForId(id);
    if (node == nullptr)
        return;
    renameWindow = std::make_unique<AlertWindow>(
        "Rename node",
        "Leave empty to reset to default",
        MessageBoxIconType::NoIcon,
        this
    );
    renameWindow->addTextEditor("name", node->properties["customName"].toString());
    renameWindow->addButton("OK", 1, KeyPress(KeyPress::returnKey));
    renameWindow->addButton("Cancel", 0, KeyPress(KeyPress::escapeKey));
    renameWindow->enterModalState(
        true,
        ModalCallbackFunction::create(
            [safe = SafePointer<GraphEditorPanel>(this),
             dialog = SafePointer<AlertWindow>(renameWindow.get()),
             generation = graph.getDocumentGeneration(),
             id](int result)
            {
                if (safe == nullptr
                    || dialog == nullptr
                    || safe->renameWindow.get() != dialog.getComponent()
                    || safe->graph.getDocumentGeneration() != generation)
                    return;
                auto name = safe->renameWindow->getTextEditorContents("name").trim();
                if (result == 1)
                    safe->showError("Rename node", safe->graph.setNodeName(id, name));
                safe->renameWindow.reset();
            }
        ),
        false
    );
}

void GraphEditorPanel::showNodeWindow(NodeID id, PluginWindow::Type type)
{
    if (graph.isParentNode(id))
        return;
    if (auto node = graph.graph.getNodeForId(id))
        if (auto* window = graph.getOrCreateWindowFor(node, type))
            window->toFront(true);
}

void GraphEditorPanel::chooseFile(
    const String& title,
    const String& wildcard,
    bool saving,
    std::function<Result(const File&)> callback
)
{
    fileChooser = std::make_unique<FileChooser>(title, File(), wildcard);
    int flags = saving ? FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting
                       : FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles;
    fileChooser->launchAsync(
        flags,
        [safe = SafePointer<GraphEditorPanel>(this),
         generation = graph.getDocumentGeneration(),
         title,
         wildcard,
         saving,
         callback = std::move(callback)](const FileChooser& chooser)
        {
            if (safe == nullptr
                || safe->fileChooser.get() != &chooser
                || safe->graph.getDocumentGeneration() != generation)
                return;
            auto file = chooser.getResult();
            if (file == File())
                return;
            if (saving)
                file = file.withFileExtension(wildcard.fromLastOccurrenceOf(".", true, false));
            safe->showError(title, callback(file));
        }
    );
}

void GraphEditorPanel::showError(const String& title, const Result& result)
{
    if (result.failed())
        messageBox = AlertWindow::showScopedAsync(
            MessageBoxOptions::makeOptionsOk(MessageBoxIconType::WarningIcon, title, result.getErrorMessage()),
            nullptr
        );
}
