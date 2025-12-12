#include "PluginGraph.h"

#include "../UI/GraphEditorPanel.h"
#include "../UI/MainHostWindow.h"
#include "InternalPlugins.h"

// Initialize static instance counter
std::atomic<int> PluginGraph::activeInstanceCount{0};

static std::unique_ptr<ScopedDPIAwarenessDisabler> makeDPIAwarenessDisablerForPlugin(const PluginDescription& desc)
{
    // return std::make_unique<ScopedDPIAwarenessDisabler>();
    return nullptr;
}

PluginGraph::PluginGraph(MainHostWindow& mw, AudioPluginFormatManager& fm, KnownPluginList& kpl)
    : FileBasedDocument(getFilenameSuffix(), getFilenameWildcard(), "Load a graph", "Save a graph")
    , formatManager(fm)
    , knownPlugins(kpl)
    , mainHostWindow(mw)
{
    activeInstanceCount.fetch_add(1, std::memory_order_relaxed);
    newDocument();
    graph.addListener(this);
}

PluginGraph::~PluginGraph()
{
    activeInstanceCount.fetch_sub(1, std::memory_order_relaxed);
    graph.removeListener(this);
    graph.removeChangeListener(this);
    graph.clear();
    // graph.updateTopology(); // Not available on base AudioProcessorGraphMT
}

PluginGraph::NodeID PluginGraph::getNextUID() noexcept
{
    return PluginGraph::NodeID(++(lastUID.uid));
}

void PluginGraph::changeListenerCallback(ChangeBroadcaster*)
{
    changed();

    for (int i = activePluginWindows.size(); --i >= 0;)
        if (!graph.getNodes().contains(activePluginWindows.getUnchecked(i)->node))
            activePluginWindows.remove(i);
}

AudioProcessorGraphMT::Node::Ptr PluginGraph::getNodeForName(const String& name) const
{
    for (auto* node : graph.getNodes())
        if (auto p = node->getProcessor())
            if (p->getName().equalsIgnoreCase(name))
                return node;

    return nullptr;
}

void PluginGraph::addPlugin(const PluginDescriptionAndPreference& desc, Point<double> pos)
{
    std::shared_ptr<ScopedDPIAwarenessDisabler> dpiDisabler = makeDPIAwarenessDisablerForPlugin(desc.pluginDescription);

    formatManager.createPluginInstanceAsync(
        desc.pluginDescription,
        graph.getSampleRate(),
        graph.getBlockSize(),
        [this, pos, dpiDisabler, useARA = desc.useARA](
            std::unique_ptr<AudioPluginInstance> instance,
            const String& error
        ) { addPluginCallback(std::move(instance), error, pos, useARA); }
    );
}

void PluginGraph::addPluginCallback(
    std::unique_ptr<AudioPluginInstance> instance,
    const String& error,
    Point<double> pos,
    PluginDescriptionAndPreference::UseARA useARA
)
{
    if (instance == nullptr)
    {
        auto options =
            MessageBoxOptions::makeOptionsOk(MessageBoxIconType::WarningIcon, TRANS("Couldn't create plugin"), error);
        messageBox = AlertWindow::showScopedAsync(options, nullptr);
    }
    else
    {
#if JUCE_PLUGINHOST_ARA && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)
        if (useARA == PluginDescriptionAndPreference::UseARA::yes && instance->getPluginDescription().hasARAExtension)
            instance = std::make_unique<ARAPluginInstanceWrapper>(std::move(instance));
#endif

        // Set parent source UUID on internal plugins that need it (e.g., ObsSourceAudioProcessor)
        setParentSourceUuidOnInternalPlugin(instance.get(), mainHostWindow.getParentSourceUuid());

        instance->enableAllBuses();

        if (auto node = graph.addNode(std::move(instance)))
        {
            node->properties.set("x", pos.x);
            node->properties.set("y", pos.y);
            node->properties.set("useARA", useARA == PluginDescriptionAndPreference::UseARA::yes);
            // graph.updateTopology(); // Not available on base AudioProcessorGraphMT
            // Note: Don't reallocate buffers here - plugins aren't prepared yet!
            // Buffers will be allocated in prepareToPlay() after graph.prepareToPlay() configures everything
            changed();
        }
    }
}

void PluginGraph::setNodePosition(NodeID nodeID, Point<double> pos)
{
    if (auto* n = graph.getNodeForId(nodeID))
    {
        n->properties.set("x", jlimit(0.0, 1.0, pos.x));
        n->properties.set("y", jlimit(0.0, 1.0, pos.y));
    }
}

Point<double> PluginGraph::getNodePosition(NodeID nodeID) const
{
    if (auto* n = graph.getNodeForId(nodeID))
        return {static_cast<double>(n->properties["x"]), static_cast<double>(n->properties["y"])};

    return {};
}

void PluginGraph::clear()
{
    closeAnyOpenPluginWindows();
    graph.clear();
    // graph.updateTopology(); // Not available on base AudioProcessorGraphMT
    changed();
}

PluginWindow* PluginGraph::getOrCreateWindowFor(AudioProcessorGraphMT::Node* node, PluginWindow::Type type)
{
    jassert(node != nullptr);

#if JUCE_IOS || JUCE_ANDROID
    closeAnyOpenPluginWindows();
#else
    for (auto* w : activePluginWindows)
        if (w->node.get() == node && w->type == type)
            return w;
#endif

    if (auto* processor = node->getProcessor())
    {
        if (auto* plugin = dynamic_cast<AudioPluginInstance*>(processor))
        {
            auto description = plugin->getPluginDescription();

            if (!plugin->hasEditor() && description.pluginFormatName == "Internal")
            {
                // Check if this is a MIDI I/O node
                if (auto* ioProcessor = dynamic_cast<AudioProcessorGraphMT::AudioGraphIOProcessor*>(processor))
                {
                    auto ioType = ioProcessor->getType();
                    if (ioType == AudioProcessorGraphMT::AudioGraphIOProcessor::midiInputNode
                        || ioType == AudioProcessorGraphMT::AudioGraphIOProcessor::midiOutputNode)
                    {
                        // Open MIDI settings window for MIDI nodes
                        mainHostWindow.getCommandManager().invokeDirectly(CommandIDs::showMidiSettings, false);
                        return nullptr;
                    }
                }

                // Open audio settings for other internal plugins without editor
                mainHostWindow.getCommandManager().invokeDirectly(CommandIDs::showAudioSettings, false);
                return nullptr;
            }

            auto localDpiDisabler = makeDPIAwarenessDisablerForPlugin(description);
            return activePluginWindows.add(new PluginWindow(node, type, activePluginWindows));
        }
    }

    return nullptr;
}

bool PluginGraph::closeAnyOpenPluginWindows()
{
    bool wasEmpty = activePluginWindows.isEmpty();
    activePluginWindows.clear();
    return !wasEmpty;
}

String PluginGraph::getDocumentTitle()
{
    if (!getFile().exists())
        return "Unnamed";

    return getFile().getFileNameWithoutExtension();
}

void PluginGraph::newDocument()
{
    clear();
    setFile({});

    graph.removeChangeListener(this);

    InternalPluginFormat internalFormat;

    jassert(internalFormat.getAllTypes().size() >= 9);

    // Create default graph: Audio Input -> Audio Output + MIDI Input -> MIDI Output
    // Index 0: Audio Input, 1: MIDI Input, 2: Audio Output, 3: MIDI Output, 8: Gain
    // Layout: Separate rows for each I/O node to avoid overlap
    addPlugin(PluginDescriptionAndPreference{internalFormat.getAllTypes()[0]}, {0.25, 0.15}); // Audio Input (top left)
    addPlugin(PluginDescriptionAndPreference{internalFormat.getAllTypes()[1]}, {0.75, 0.25}); // MIDI Input (top right)
    // addPlugin(PluginDescriptionAndPreference{internalFormat.getAllTypes()[9]}, {0.5, 0.5});   // Gain (center)
    addPlugin(
        PluginDescriptionAndPreference{internalFormat.getAllTypes()[2]},
        {0.25, 0.85}
    ); // Audio Output (bottom left)
    addPlugin(
        PluginDescriptionAndPreference{internalFormat.getAllTypes()[3]},
        {0.75, 0.75}
    ); // MIDI Output (bottom right)

    MessageManager::callAsync(
        [this]
        {
            auto nodes = graph.getNodes();
            if (nodes.size() >= 4)
            {
                graph.addConnection({
                    {nodes[0]->nodeID, 0},
                    {nodes[2]->nodeID, 0}
                }); // Left
                graph.addConnection({
                    {nodes[0]->nodeID, 1},
                    {nodes[2]->nodeID, 1}
                }); // Right
            }

            graph.rebuild();

            graph.addChangeListener(this);
            setChangedFlag(false);
        }
    );
}

Result PluginGraph::loadDocument(const File& file)
{
    if (auto xml = parseXMLIfTagMatches(file, "FILTERGRAPH"))
    {
        graph.removeChangeListener(this);
        restoreFromXml(*xml);

        MessageManager::callAsync(
            [this]
            {
                setChangedFlag(false);
                graph.addChangeListener(this);
            }
        );

        return Result::ok();
    }

    return Result::fail("Not a valid graph file");
}

Result PluginGraph::saveDocument(const File& file)
{
    auto xml = createXml();

    if (!xml->writeTo(file, {}))
        return Result::fail("Couldn't write to the file");

    return Result::ok();
}

File PluginGraph::getLastDocumentOpened()
{
    RecentlyOpenedFilesList recentFiles;
    recentFiles.restoreFromString(
        mainHostWindow.getAppProperties().getUserSettings()->getValue("recentFilterGraphFiles")
    );

    return recentFiles.getFile(0);
}

void PluginGraph::setLastDocumentOpened(const File& file)
{
    RecentlyOpenedFilesList recentFiles;
    recentFiles.restoreFromString(
        mainHostWindow.getAppProperties().getUserSettings()->getValue("recentFilterGraphFiles")
    );

    recentFiles.addFile(file);

    mainHostWindow.getAppProperties().getUserSettings()->setValue("recentFilterGraphFiles", recentFiles.toString());
}

static void readBusLayoutFromXml(
    AudioProcessor::BusesLayout& busesLayout,
    AudioProcessor& plugin,
    const XmlElement& xml,
    bool isInput
)
{
    auto& targetBuses = (isInput ? busesLayout.inputBuses : busesLayout.outputBuses);
    int maxNumBuses = 0;

    if (auto* buses = xml.getChildByName(isInput ? "INPUTS" : "OUTPUTS"))
    {
        for (auto* e : buses->getChildWithTagNameIterator("BUS"))
        {
            const int busIdx = e->getIntAttribute("index");
            maxNumBuses = jmax(maxNumBuses, busIdx + 1);

            // the number of buses on busesLayout may not be in sync with the plugin after adding buses
            // because adding an input bus could also add an output bus
            for (int actualIdx = plugin.getBusCount(isInput) - 1; actualIdx < busIdx; ++actualIdx)
                if (!plugin.addBus(isInput))
                    return;

            for (int actualIdx = targetBuses.size() - 1; actualIdx < busIdx; ++actualIdx)
                targetBuses.add(plugin.getChannelLayoutOfBus(isInput, busIdx));

            auto layout = e->getStringAttribute("layout");

            if (layout.isNotEmpty())
                targetBuses.getReference(busIdx) = AudioChannelSet::fromAbbreviatedString(layout);
        }
    }

    // if the plugin has more buses than specified in the xml, then try to remove them!
    while (maxNumBuses < targetBuses.size())
    {
        if (!plugin.removeBus(isInput))
            return;

        targetBuses.removeLast();
    }
}

static XmlElement* createBusLayoutXml(const AudioProcessor::BusesLayout& layout, const bool isInput)
{
    auto& buses = isInput ? layout.inputBuses : layout.outputBuses;

    auto* xml = new XmlElement(isInput ? "INPUTS" : "OUTPUTS");

    for (int busIdx = 0; busIdx < buses.size(); ++busIdx)
    {
        auto& set = buses.getReference(busIdx);

        auto* bus = xml->createNewChildElement("BUS");
        bus->setAttribute("index", busIdx);
        bus->setAttribute("layout", set.isDisabled() ? "disabled" : set.getSpeakerArrangementAsString());
    }

    return xml;
}

static XmlElement* createNodeXml(AudioProcessorGraphMT::Node* const node) noexcept
{
    if (auto* plugin = dynamic_cast<AudioPluginInstance*>(node->getProcessor()))
    {
        auto e = new XmlElement("FILTER");

        e->setAttribute("uid", (int)node->nodeID.uid);
        e->setAttribute("x", node->properties["x"].toString());
        e->setAttribute("y", node->properties["y"].toString());
        e->setAttribute("useARA", node->properties["useARA"].toString());

        // Save custom name if set
        if (node->properties.contains("customName"))
            e->setAttribute("customName", node->properties["customName"].toString());

        for (int i = 0; i < (int)PluginWindow::Type::numTypes; ++i)
        {
            auto type = (PluginWindow::Type)i;

            if (node->properties.contains(PluginWindow::getOpenProp(type)))
            {
                e->setAttribute(
                    PluginWindow::getLastXProp(type),
                    node->properties[PluginWindow::getLastXProp(type)].toString()
                );
                e->setAttribute(
                    PluginWindow::getLastYProp(type),
                    node->properties[PluginWindow::getLastYProp(type)].toString()
                );
                e->setAttribute(
                    PluginWindow::getOpenProp(type),
                    node->properties[PluginWindow::getOpenProp(type)].toString()
                );
            }
        }

        {
            PluginDescription pd;
            plugin->fillInPluginDescription(pd);
            e->addChildElement(pd.createXml().release());
        }

        {
            MemoryBlock m;
            node->getProcessor()->getStateInformation(m);
            e->createNewChildElement("STATE")->addTextElement(m.toBase64Encoding());
        }

        auto layout = plugin->getBusesLayout();

        auto layouts = e->createNewChildElement("LAYOUT");
        layouts->addChildElement(createBusLayoutXml(layout, true));
        layouts->addChildElement(createBusLayoutXml(layout, false));

        return e;
    }

    jassertfalse;
    return nullptr;
}

void PluginGraph::createNodeFromXml(const XmlElement& xml)
{
    PluginDescriptionAndPreference pd;
    const auto nodeUsesARA = xml.getBoolAttribute("useARA");

    for (auto* e : xml.getChildIterator())
    {
        if (pd.pluginDescription.loadFromXml(*e))
        {
            pd.useARA =
                nodeUsesARA ? PluginDescriptionAndPreference::UseARA::yes : PluginDescriptionAndPreference::UseARA::no;
            break;
        }
    }

    auto createInstanceWithFallback = [&]() -> std::unique_ptr<AudioPluginInstance>
    {
        auto createInstance =
            [this](const PluginDescriptionAndPreference& description) -> std::unique_ptr<AudioPluginInstance>
        {
            String errorMessage;

            auto localDpiDisabler = makeDPIAwarenessDisablerForPlugin(description.pluginDescription);

            auto instance = formatManager.createPluginInstance(
                description.pluginDescription,
                graph.getSampleRate(),
                graph.getBlockSize(),
                errorMessage
            );

#if JUCE_PLUGINHOST_ARA && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)
            if (instance
                && description.useARA == PluginDescriptionAndPreference::UseARA::yes
                && description.pluginDescription.hasARAExtension)
            {
                return std::make_unique<ARAPluginInstanceWrapper>(std::move(instance));
            }
#endif

            return instance;
        };

        if (auto instance = createInstance(pd))
            return instance;

        const auto allFormats = formatManager.getFormats();
        const auto matchingFormat = std::find_if(
            allFormats.begin(),
            allFormats.end(),
            [&](const AudioPluginFormat* f) { return f->getName() == pd.pluginDescription.pluginFormatName; }
        );

        if (matchingFormat == allFormats.end())
            return nullptr;

        const auto plugins = knownPlugins.getTypesForFormat(**matchingFormat);
        const auto matchingPlugin = std::find_if(
            plugins.begin(),
            plugins.end(),
            [&](const PluginDescription& desc) { return pd.pluginDescription.uniqueId == desc.uniqueId; }
        );

        if (matchingPlugin == plugins.end())
            return nullptr;

        return createInstance(PluginDescriptionAndPreference{*matchingPlugin});
    };

    if (auto instance = createInstanceWithFallback())
    {
        // Set parent source UUID on internal plugins that need it (e.g., ObsSourceAudioProcessor)
        setParentSourceUuidOnInternalPlugin(instance.get(), mainHostWindow.getParentSourceUuid());

        if (auto* layoutEntity = xml.getChildByName("LAYOUT"))
        {
            auto layout = instance->getBusesLayout();

            readBusLayoutFromXml(layout, *instance, *layoutEntity, true);
            readBusLayoutFromXml(layout, *instance, *layoutEntity, false);

            instance->setBusesLayout(layout);
        }

        if (auto node = graph.addNode(std::move(instance), NodeID((uint32)xml.getIntAttribute("uid"))))
        {
            if (auto* state = xml.getChildByName("STATE"))
            {
                MemoryBlock m;
                m.fromBase64Encoding(state->getAllSubText());

                node->getProcessor()->setStateInformation(m.getData(), (int)m.getSize());
            }

            node->properties.set("x", xml.getDoubleAttribute("x"));
            node->properties.set("y", xml.getDoubleAttribute("y"));
            node->properties.set("useARA", xml.getBoolAttribute("useARA"));

            // Load custom name if present
            if (xml.hasAttribute("customName"))
                node->properties.set("customName", xml.getStringAttribute("customName"));

            for (int i = 0; i < (int)PluginWindow::Type::numTypes; ++i)
            {
                auto type = (PluginWindow::Type)i;

                if (xml.hasAttribute(PluginWindow::getOpenProp(type)))
                {
                    node->properties.set(
                        PluginWindow::getLastXProp(type),
                        xml.getIntAttribute(PluginWindow::getLastXProp(type))
                    );
                    node->properties.set(
                        PluginWindow::getLastYProp(type),
                        xml.getIntAttribute(PluginWindow::getLastYProp(type))
                    );
                    node->properties.set(
                        PluginWindow::getOpenProp(type),
                        xml.getIntAttribute(PluginWindow::getOpenProp(type))
                    );

                    if (node->properties[PluginWindow::getOpenProp(type)])
                    {
                        jassert(node->getProcessor() != nullptr);

                        if (auto w = getOrCreateWindowFor(node.get(), type))
                            w->toFront(true);
                    }
                }
            }
        }
    }
}

std::unique_ptr<XmlElement> PluginGraph::createXml() const
{
    auto xml = std::make_unique<XmlElement>("FILTERGRAPH");

    for (auto* node : graph.getNodes())
        xml->addChildElement(createNodeXml(node));

    for (auto& connection : graph.getConnections())
    {
        auto e = xml->createNewChildElement("CONNECTION");

        e->setAttribute("srcFilter", (int)connection.source.nodeID.uid);
        e->setAttribute("srcChannel", connection.source.channelIndex);
        e->setAttribute("dstFilter", (int)connection.destination.nodeID.uid);
        e->setAttribute("dstChannel", connection.destination.channelIndex);
    }

    return xml;
}

void PluginGraph::restoreFromXml(const XmlElement& xml)
{
    clear();

    for (auto* e : xml.getChildWithTagNameIterator("FILTER"))
    {
        createNodeFromXml(*e);
        changed();
    }

    for (auto* e : xml.getChildWithTagNameIterator("CONNECTION"))
    {
        graph.addConnection({
            {NodeID((uint32)e->getIntAttribute("srcFilter")), e->getIntAttribute("srcChannel")},
            {NodeID((uint32)e->getIntAttribute("dstFilter")), e->getIntAttribute("dstChannel")}
        });
    }

    // graph.updateTopology(); // Not available on base AudioProcessorGraphMT

    graph.removeIllegalConnections();
}

File PluginGraph::getDefaultGraphDocumentOnMobile()
{
    auto persistantStorageLocation = File::getSpecialLocation(File::userApplicationDataDirectory);
    return persistantStorageLocation.getChildFile("state.filtergraph");
}

void PluginGraph::processMidiInput(juce::MidiBuffer& midiMessages, int numSamples, double sampleRate)
{
    // Get MIDI input from MidiServer through MainHostWindow's MidiClient
    mainHostWindow.getMidiClient().getPendingMidi(midiMessages, numSamples, sampleRate);
}

void PluginGraph::processMidiOutput(const juce::MidiBuffer& midiMessages)
{
    // Send MIDI output to MidiServer through MainHostWindow's MidiClient
    mainHostWindow.getMidiClient().sendMidi(midiMessages);
}
