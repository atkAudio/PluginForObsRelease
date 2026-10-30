#include "PluginGraph.h"
#include "missing_plugin.h"
#include "InternalPlugins/ParentSource.h"
#include "InternalPlugins/Transport.h"
#include "InternalPlugins/ChordAnalyzerPlugin.h"

#include <atkaudio/ObsAudioChannelSet.h>
#include <atkaudio/midi_control/last_touched_parameter_tracker.h>
#include <utility>

static XmlElement* createNodeXml(AudioProcessorGraphMT::Node* const node) noexcept;

// Initialize static instance counter
std::atomic<int> PluginGraph::activeInstanceCount{0};

static void applyObsDefaultLayoutToNewNode(AudioPluginInstance& instance)
{
    auto preferredLayout = getDefaultObsAudioChannelSet();
    auto currentLayout = instance.getBusesLayout();
    auto layoutChanged = false;

    auto updateMainBus = [&](bool isInput)
    {
        if (instance.getBusCount(isInput) <= 0)
            return;

        auto& buses = isInput ? currentLayout.inputBuses : currentLayout.outputBuses;
        if (buses.isEmpty())
            return;

        auto existingLayout = instance.getChannelLayoutOfBus(isInput, 0);
        if (existingLayout.isDisabled() || existingLayout == preferredLayout)
            return;

        buses.getReference(0) = preferredLayout;
        layoutChanged = true;
    };

    updateMainBus(true);
    updateMainBus(false);

    if (layoutChanged && instance.checkBusesLayoutSupported(currentLayout))
        instance.setBusesLayout(currentLayout);
}

static bool applyParentSourceLayout(atk::ParentSourceProcessor& processor, atk::ParentSourceBridge* bridge)
{
    auto layout =
        bridge != nullptr ? getChannelSetForCount(bridge->getParentChannels()) : getDefaultObsAudioChannelSet();
    bool isInput = processor.getRole() == atk::ParentSourceProcessor::output;
    return processor.getChannelLayoutOfBus(isInput, 0) != layout && processor.setChannelLayoutOfBus(isInput, 0, layout);
}

PluginGraph::PluginGraph(
    AudioPluginFormatManager& fm,
    KnownPluginList& kpl,
    PropertiesFile* properties,
    NativeEditorHost* host
)
    : FileBasedDocument(getFilenameSuffix(), getFilenameWildcard(), "Load a graph", "Save a graph")
    , nativeEditorHost(host)
    , formatManager(fm)
    , knownPlugins(kpl)
    , settings(properties)
{
    activeInstanceCount.fetch_add(1, std::memory_order_relaxed);
    graph.addListener(this);
    graph.addChangeListener(this);
}

PluginGraph::~PluginGraph()
{
    if (boundChordNode != nullptr)
        static_cast<ChordAnalyzerPlugin*>(boundChordNode->getProcessor())->bind(nullptr, false);
    boundChordNode = nullptr;
    if (boundTransportNode != nullptr)
        static_cast<TransportProcessor*>(boundTransportNode->getProcessor())->bind(nullptr);
    boundTransportNode = nullptr;
    if (parentSourceBridge != nullptr)
        parentSourceBridge->removeChangeListener(this);
    lifetimeToken->store(false, std::memory_order_release);
    for (const auto& entry : trackedNodeProcessors)
        if (entry.second != nullptr)
            if (auto* processor = entry.second->getProcessor())
                atk::LastTouchedParameterTracker::getInstance().unregisterProcessor(*processor);
    trackedNodeProcessors.clear();

    activeInstanceCount.fetch_sub(1, std::memory_order_relaxed);
    // Editor teardown owns plugin window cleanup and may already have destroyed
    // MainHostWindow before PluginGraph is deleted.
    graph.removeListener(this);
    graph.removeChangeListener(this);
    graph.clear();
}

PluginGraph::NodeID PluginGraph::getNextUID() noexcept
{
    do
    {
        ++lastUID.uid;
    } while (graph.getNodeForId(lastUID) != nullptr);
    return lastUID;
}

void PluginGraph::changeListenerCallback(ChangeBroadcaster* source)
{
    if (source == parentSourceBridge && parentSourceBridge != nullptr)
    {
        auto connections = graph.getConnections();
        bool changedLayout = false;
        for (auto* node : graph.getNodes())
            if (auto* processor = dynamic_cast<atk::ParentSourceProcessor*>(node->getProcessor()))
                changedLayout = applyParentSourceLayout(*processor, parentSourceBridge) || changedLayout;
        if (changedLayout)
            for (auto& connection : connections)
                if (isParentNode(connection.source.nodeID) || isParentNode(connection.destination.nodeID))
                {
                    graph.removeConnection(connection);
                    pendingConnections.addOrDefer(graph, connection);
                }
    }
    pendingConnections.retry(graph);
    syncLastTouchedTracker();
    sendChangeMessage();

    if (nativeEditorHost != nullptr)
        nativeEditorHost->pruneStalePluginWindows(graph);
}

void PluginGraph::refreshLastTouchedTrackerOwnership()
{
    syncLastTouchedTracker();
}

void PluginGraph::setParentSourceBridge(atk::ParentSourceBridge* bridge)
{
    if (parentSourceBridge != nullptr)
        parentSourceBridge->removeChangeListener(this);
    parentSourceBridge = bridge;
    for (auto* node : graph.getNodes())
        if (auto* processor = dynamic_cast<atk::ParentSourceProcessor*>(node->getProcessor()))
            processor->setBridge(bridge);
    if (bridge != nullptr)
        bridge->addChangeListener(this);
    ensureParentSourceNodes();
    sendChangeMessage();
}

bool PluginGraph::isParentNode(NodeID id) const
{
    auto* node = graph.getNodeForId(id);
    return node != nullptr && dynamic_cast<atk::ParentSourceProcessor*>(node->getProcessor()) != nullptr;
}

bool PluginGraph::isNodeVisible(NodeID id) const
{
    return graph.getNodeForId(id) != nullptr
        && (!isParentNode(id)
            || (parentSourceBridge != nullptr && parentSourceBridge->getMode() == atk::ParentSourceBridge::hardware));
}

void PluginGraph::ensureParentSourceNodes()
{
    if (parentSourceBridge == nullptr)
        return;
    for (auto role : {atk::ParentSourceProcessor::input, atk::ParentSourceProcessor::output})
    {
        atk::ParentSourceProcessor* existing = nullptr;
        for (auto* node : graph.getNodes())
            if (auto* processor = dynamic_cast<atk::ParentSourceProcessor*>(node->getProcessor());
                processor != nullptr && processor->getRole() == role)
                existing = processor;
        if (existing != nullptr)
        {
            existing->setBridge(parentSourceBridge);
            applyParentSourceLayout(*existing, parentSourceBridge);
            continue;
        }
        auto processor = std::make_unique<atk::ParentSourceProcessor>(role);
        processor->setBridge(parentSourceBridge);
        applyParentSourceLayout(*processor, parentSourceBridge);
        if (auto node = graph.addNode(std::move(processor), getNextUID()))
        {
            node->properties.set("x", 0.5);
            node->properties.set("y", role == atk::ParentSourceProcessor::input ? 0.35 : 0.65);
        }
    }
}

void PluginGraph::syncLastTouchedTracker()
{
    syncTransportNode();
    syncChordAnalyzer();
    if (nativeEditorHost == nullptr)
        return;
    std::unordered_map<uint32_t, AudioProcessorGraphMT::Node::Ptr> currentProcessors;
    const auto ownerSourceUuid =
        nativeEditorHost != nullptr ? juce::String(nativeEditorHost->getParentSourceUuid()) : String();
    const auto ownerFilterName =
        nativeEditorHost != nullptr ? juce::String(nativeEditorHost->getOwnerFilterName()) : String();
    const auto ownerChanged = trackedOwnerSourceUuid != ownerSourceUuid || trackedOwnerFilterName != ownerFilterName;

    for (auto* node : graph.getNodes())
    {
        if (node == nullptr)
            continue;

        auto* processor = node->getProcessor();
        if (processor == nullptr)
            continue;

        if (processor->getParameters().isEmpty())
            continue;

        currentProcessors.emplace(node->nodeID.uid, node);
    }

    for (const auto& trackedEntry : trackedNodeProcessors)
    {
        auto* trackedProcessor = trackedEntry.second != nullptr ? trackedEntry.second->getProcessor() : nullptr;
        if (trackedProcessor == nullptr)
            continue;

        auto current = currentProcessors.find(trackedEntry.first);
        if (ownerChanged || current == currentProcessors.end() || current->second != trackedEntry.second)
            atk::LastTouchedParameterTracker::getInstance().unregisterProcessor(*trackedProcessor);
    }

    for (const auto& currentEntry : currentProcessors)
    {
        auto tracked = trackedNodeProcessors.find(currentEntry.first);
        if (ownerChanged || tracked == trackedNodeProcessors.end() || tracked->second != currentEntry.second)
            atk::LastTouchedParameterTracker::getInstance()
                .registerProcessor(*currentEntry.second->getProcessor(), ownerSourceUuid, ownerFilterName);
    }

    trackedNodeProcessors = std::move(currentProcessors);
    trackedOwnerSourceUuid = ownerSourceUuid;
    trackedOwnerFilterName = ownerFilterName;
}

AudioProcessorGraphMT::Node::Ptr PluginGraph::getNodeForName(const String& name) const
{
    for (auto* node : graph.getNodes())
        if (auto p = node->getProcessor())
            if ((node->properties["customName"].toString().isNotEmpty() ? node->properties["customName"].toString()
                                                                        : p->getName())
                    .equalsIgnoreCase(name))
                return node;

    return nullptr;
}

void PluginGraph::addPlugin(const PluginDescriptionAndPreference& desc, Point<double> pos, PluginAddedCallback callback)
{
    if (atk::ParentSourceProcessor::isParentDescription(desc.pluginDescription))
    {
        if (callback)
            callback(Result::fail("Parent source nodes are managed by the host"), {});
        return;
    }
    auto active = lifetimeToken;
    auto generation = documentGeneration;

    formatManager.createPluginInstanceAsync(
        desc.pluginDescription,
        graph.getSampleRate(),
        graph.getBlockSize(),
        [this, active, generation, pos, callback = std::move(callback), useARA = desc.useARA](
            std::unique_ptr<AudioPluginInstance> instance,
            const String& error
        )
        {
            if (active->load(std::memory_order_acquire) && generation == documentGeneration)
                addPluginCallback(std::move(instance), error, pos, useARA, callback);
        }
    );
}

void PluginGraph::addPluginCallback(
    std::unique_ptr<AudioPluginInstance> instance,
    const String& error,
    Point<double> pos,
    PluginDescriptionAndPreference::UseARA useARA,
    const PluginAddedCallback& callback
)
{
    String creationError = error;
    if (instance != nullptr
        && dynamic_cast<ChordAnalyzerPlugin*>(instance.get()) != nullptr
        && getChordAnalyzer() != nullptr)
    {
        instance = nullptr;
        creationError = "Only one Chord Analyzer is allowed per host";
    }
    if (instance != nullptr
        && dynamic_cast<TransportProcessor*>(instance.get()) != nullptr
        && getTransportProcessor() != nullptr)
    {
        instance = nullptr;
        creationError = "Only one Transport is allowed per host";
    }
    if (instance == nullptr)
    {
        if (callback)
        {
            callback(Result::fail(creationError), {});
            return;
        }
        if (nativeEditorHost == nullptr)
            return;
        auto options = MessageBoxOptions::makeOptionsOk(
            MessageBoxIconType::WarningIcon,
            TRANS("Couldn't create plugin"),
            creationError
        );
        messageBox = AlertWindow::showScopedAsync(options, nullptr);
    }
    else
    {
        if (auto* vst3 = instance->getVST3Client())
            vst3->setProcessContextProvider(vst3ChordContext);
#if JUCE_PLUGINHOST_ARA && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)
        if (useARA == PluginDescriptionAndPreference::UseARA::yes && instance->getPluginDescription().hasARAExtension)
            instance = std::make_unique<ARAPluginInstanceWrapper>(std::move(instance));
#endif

        // Set parent source UUID on internal plugins that need it (e.g., ObsSourceAudioProcessor)
        if (nativeEditorHost != nullptr)
            nativeEditorHost->bindPluginToOwner(instance.get());

        if (auto* controller = dynamic_cast<TransportProcessor*>(instance.get()))
        {
            controller->setSettings(getTransportSettings());
            controller->bind(this);
        }
        instance->enableAllBuses();
        applyObsDefaultLayoutToNewNode(*instance);

        Edit edit(*this, "Add plugin");
        if (auto node = graph.addNode(std::move(instance), getNextUID()))
        {
            node->properties.set("x", pos.x);
            node->properties.set("y", pos.y);
            node->properties.set("useARA", useARA == PluginDescriptionAndPreference::UseARA::yes);
            // Note: Don't reallocate buffers here - plugins aren't prepared yet!
            // Buffers will be allocated in prepareToPlay() after graph.prepareToPlay() configures everything
            syncLastTouchedTracker();
            changed();
            if (callback)
                callback(Result::ok(), node->nodeID);
        }
        else if (callback)
            callback(Result::fail("Unable to add plugin to the graph"), {});
    }
}

Result PluginGraph::setNodePosition(NodeID nodeID, Point<double> pos)
{
    if (auto* n = graph.getNodeForId(nodeID))
    {
        Edit edit(*this, "Move");
        n->properties.set("x", pos.x);
        n->properties.set("y", pos.y);
        return Result::ok();
    }
    return Result::fail("Node does not exist");
}

Point<double> PluginGraph::getNodePosition(NodeID nodeID) const
{
    if (auto* n = graph.getNodeForId(nodeID))
        return {static_cast<double>(n->properties["x"]), static_cast<double>(n->properties["y"])};

    return {};
}

void PluginGraph::clear(bool resetTransport)
{
    if (boundChordNode != nullptr)
        static_cast<ChordAnalyzerPlugin*>(boundChordNode->getProcessor())->bind(nullptr, false);
    boundChordNode = nullptr;
    chordMidiConnections.clear();
    chordMidiBypass.clear();
    if (boundTransportNode != nullptr)
        static_cast<TransportProcessor*>(boundTransportNode->getProcessor())->bind(nullptr);
    boundTransportNode = nullptr;
    if (resetTransport)
        transportSettings.set({});
    if (parentSourceBridge != nullptr)
        parentSourceBridge->reset();
    ++documentGeneration;
    ++documentRevision;
    activeEdit.reset();
    undoManager.clearUndoHistory();
    groups.clear();
    resettingDocument = false;
    graph.addChangeListener(this);
    closeAnyOpenPluginWindows();
    pendingConnections.clear();
    graph.clear();
    syncLastTouchedTracker();
    changed();
    sendChangeMessage();
}

PluginWindow* PluginGraph::getOrCreateWindowFor(AudioProcessorGraphMT::Node* node, PluginWindow::Type type)
{
    if (node == nullptr || dynamic_cast<MissingPlugin*>(node->getProcessor()) != nullptr)
        return nullptr;
    return nativeEditorHost != nullptr ? nativeEditorHost->getOrCreatePluginWindowFor(node, type) : nullptr;
}

bool PluginGraph::closeAnyOpenPluginWindows()
{
    return nativeEditorHost != nullptr && nativeEditorHost->closeAnyOpenPluginWindows();
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
    resettingDocument = true;

    graph.removeChangeListener(this);

    using IoProcessor = AudioProcessorGraphMT::AudioGraphIOProcessor;
    IoProcessor::IODeviceType types[] = {
        IoProcessor::audioInputNode,
        IoProcessor::midiInputNode,
        IoProcessor::audioOutputNode,
        IoProcessor::midiOutputNode
    };

    struct InitialNodes
    {
        int remaining = 4;
        NodeID ids[4];
    };

    auto initial = std::make_shared<InitialNodes>();
    Point<double> positions[] = {
        {0.25, 0.15},
        {0.75, 0.25},
        {0.25, 0.85},
        {0.75, 0.75}
    };
    for (int index = 0; index < 4; ++index)
        addPlugin(
            PluginDescriptionAndPreference{IoProcessor(types[index]).getPluginDescription()},
            positions[index],
            [this, active = lifetimeToken, generation = documentGeneration, initial, index](const Result&, NodeID id)
            {
                if (!active->load(std::memory_order_acquire) || generation != documentGeneration)
                    return;
                initial->ids[index] = id;
                if (--initial->remaining != 0)
                    return;
                MessageManager::callAsync(
                    [this, active, generation, initial]
                    {
                        if (!active->load(std::memory_order_acquire) || generation != documentGeneration)
                            return;
                        if (initial->ids[0].uid != 0 && initial->ids[2].uid != 0)
                            for (int channel = 0; channel < 2; ++channel)
                                graph.addConnection({
                                    {initial->ids[0], channel},
                                    {initial->ids[2], channel}
                                });
                        graph.rebuild();
                        ensureParentSourceNodes();
                        graph.addChangeListener(this);
                        resettingDocument = false;
                        undoManager.clearUndoHistory();
                        setChangedFlag(false);
                        sendChangeMessage();
                    }
                );
            }
        );
}

Result PluginGraph::loadDocument(const File& file)
{
    if (auto xml = parseXMLIfTagMatches(file, "FILTERGRAPH"))
    {
        graph.removeChangeListener(this);
        auto result = restoreFromXml(*xml);
        if (result.failed())
        {
            graph.addChangeListener(this);
            return result;
        }

        MessageManager::callAsync(
            [this, active = lifetimeToken, generation = documentGeneration, revision = getDocumentRevision()]
            {
                if (!active->load(std::memory_order_acquire) || generation != documentGeneration)
                    return;
                if (revision == getDocumentRevision())
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
    if (settings == nullptr)
        return {};
    RecentlyOpenedFilesList recentFiles;
    recentFiles.restoreFromString(settings->getValue("recentFilterGraphFiles"));

    return recentFiles.getFile(0);
}

void PluginGraph::setLastDocumentOpened(const File& file)
{
    if (settings == nullptr)
        return;
    RecentlyOpenedFilesList recentFiles;
    recentFiles.restoreFromString(settings->getValue("recentFilterGraphFiles"));

    recentFiles.addFile(file);

    settings->setValue("recentFilterGraphFiles", recentFiles.toString());
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
        auto* missing = dynamic_cast<MissingPlugin*>(plugin);
        auto e = missing != nullptr ? new XmlElement(missing->originalXml) : new XmlElement("FILTER");

        e->setAttribute("uid", (int)node->nodeID.uid);
        e->setAttribute("x", node->properties["x"].toString());
        e->setAttribute("y", node->properties["y"].toString());
        e->setAttribute("useARA", (bool)node->properties["useARA"]);
        e->setAttribute("bypassed", node->isBypassed());
        e->setAttribute("acceptsMidi", plugin->acceptsMidi());
        e->setAttribute("producesMidi", plugin->producesMidi());

        // Save custom name if set
        if (node->properties.contains("customName"))
            e->setAttribute("customName", node->properties["customName"].toString());
        else
            e->removeAttribute("customName");

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

        if (missing == nullptr)
        {
            PluginDescription pd;
            plugin->fillInPluginDescription(pd);
            e->addChildElement(pd.createXml().release());
        }

        if (missing == nullptr)
        {
            MemoryBlock m;
            node->getProcessor()->getStateInformation(m);
            e->createNewChildElement("STATE")->addTextElement(m.toBase64Encoding());
        }

        if (missing == nullptr)
        {
            auto layout = plugin->getBusesLayout();
            auto layouts = e->createNewChildElement("LAYOUT");
            layouts->addChildElement(createBusLayoutXml(layout, true));
            layouts->addChildElement(createBusLayoutXml(layout, false));
        }

        return e;
    }

    jassertfalse;
    return nullptr;
}

AudioProcessorGraphMT::Node::Ptr
PluginGraph::createNodeFromXml(const XmlElement& xml, NodeID nodeID, bool restoreWindows, const XmlElement* sourceGraph)
{
    PluginDescriptionAndPreference pd;
    const auto nodeUsesARA = xml.getBoolAttribute("useARA");
    bool hasDescription = false;

    for (auto* e : xml.getChildIterator())
    {
        if (pd.pluginDescription.loadFromXml(*e))
        {
            hasDescription = true;
            pd.useARA =
                nodeUsesARA ? PluginDescriptionAndPreference::UseARA::yes : PluginDescriptionAndPreference::UseARA::no;
            break;
        }
    }
    if (!hasDescription)
        return nullptr;
    if (auto* saved = xml.getChildByName("STATE"))
    {
        MemoryBlock state;
        if (!state.fromBase64Encoding(saved->getAllSubText()))
            return nullptr;
    }
    String creationError;

    auto createInstanceWithFallback = [&]() -> std::unique_ptr<AudioPluginInstance>
    {
        if (ChordAnalyzerPlugin::isDescription(pd.pluginDescription))
        {
            if (getChordAnalyzer() != nullptr)
            {
                pluginLoadErrors.add("Only one Chord Analyzer is allowed per host");
                return nullptr;
            }
            return std::make_unique<ChordAnalyzerPlugin>();
        }
        if (TransportProcessor::isTransportDescription(pd.pluginDescription))
        {
            if (getTransportProcessor() != nullptr)
                return nullptr;
            return std::make_unique<TransportProcessor>();
        }
        if (atk::ParentSourceProcessor::isParentDescription(pd.pluginDescription))
            return std::make_unique<atk::ParentSourceProcessor>(
                pd.pluginDescription.name == "Parent Source In" ? atk::ParentSourceProcessor::input
                                                                : atk::ParentSourceProcessor::output
            );
        auto createInstance = [this, &creationError](
                                  const PluginDescriptionAndPreference& description
                              ) -> std::unique_ptr<AudioPluginInstance>
        {
            String errorMessage;

            auto instance = formatManager.createPluginInstance(
                description.pluginDescription,
                graph.getSampleRate(),
                graph.getBlockSize(),
                errorMessage
            );
            if (instance == nullptr && errorMessage.isNotEmpty())
                creationError = errorMessage;

            if (instance != nullptr)
                if (auto* vst3 = instance->getVST3Client())
                    vst3->setProcessContextProvider(vst3ChordContext);

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

    auto instance = createInstanceWithFallback();
    if (instance == nullptr && pd.pluginDescription.pluginFormatName != "Internal")
    {
        bool midiInput = false;
        bool midiOutput = false;
        if (sourceGraph != nullptr)
            for (auto* connection : sourceGraph->getChildWithTagNameIterator("CONNECTION"))
            {
                midiInput = midiInput
                         || (connection->getIntAttribute("dstFilter") == xml.getIntAttribute("uid")
                             && connection->getIntAttribute("dstChannel") == AudioProcessorGraphMT::midiChannelIndex);
                midiOutput = midiOutput
                          || (connection->getIntAttribute("srcFilter") == xml.getIntAttribute("uid")
                              && connection->getIntAttribute("srcChannel") == AudioProcessorGraphMT::midiChannelIndex);
            }
        instance = std::make_unique<MissingPlugin>(
            xml,
            pd.pluginDescription,
            creationError.isNotEmpty() ? creationError : "Plugin or format is unavailable",
            xml.getBoolAttribute("acceptsMidi", midiInput),
            xml.getBoolAttribute("producesMidi", midiOutput)
        );
        pluginLoadErrors.add(pd.pluginDescription.name + " (" + pd.pluginDescription.pluginFormatName + ")");
    }
    if (instance != nullptr)
    {
        if (auto* controller = dynamic_cast<TransportProcessor*>(instance.get()))
            controller->bind(this, !xml.getBoolAttribute("bypassed", false));
        auto* parentProcessor = dynamic_cast<atk::ParentSourceProcessor*>(instance.get());
        if (parentProcessor != nullptr)
            parentProcessor->setBridge(parentSourceBridge);
        // Set parent source UUID on internal plugins that need it (e.g., ObsSourceAudioProcessor)
        if (nativeEditorHost != nullptr)
            nativeEditorHost->bindPluginToOwner(instance.get());

        if (auto* layoutEntity = xml.getChildByName("LAYOUT");
            layoutEntity != nullptr && dynamic_cast<MissingPlugin*>(instance.get()) == nullptr)
        {
            auto layout = instance->getBusesLayout();

            readBusLayoutFromXml(layout, *instance, *layoutEntity, true);
            readBusLayoutFromXml(layout, *instance, *layoutEntity, false);

            instance->setBusesLayout(layout);
        }

        if (parentProcessor != nullptr)
            applyParentSourceLayout(*parentProcessor, parentSourceBridge);

        if (nodeID.uid == 0)
            nodeID = getNextUID();
        lastUID.uid = jmax(lastUID.uid, nodeID.uid);
        if (auto node = graph.addNode(std::move(instance), nodeID))
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
            node->setBypassed(parentProcessor == nullptr && xml.getBoolAttribute("bypassed", false));

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

                    if (restoreWindows && node->properties[PluginWindow::getOpenProp(type)])
                    {
                        jassert(node->getProcessor() != nullptr);

                        if (auto w = getOrCreateWindowFor(node.get(), type))
                            w->toFront(true);
                    }
                }
            }
            return node;
        }
    }
    pluginLoadErrors.add(pd.pluginDescription.name + " (" + pd.pluginDescription.pluginFormatName + ")");
    return nullptr;
}

std::unique_ptr<XmlElement> PluginGraph::createXml() const
{
    auto xml = std::make_unique<XmlElement>("FILTERGRAPH");

    for (auto* node : graph.getNodes())
        xml->addChildElement(createNodeXml(node));

    auto activeConnections = graph.getConnections();
    std::set<Connection> connections(activeConnections.begin(), activeConnections.end());
    const auto& pending = pendingConnections.get();
    connections.insert(pending.begin(), pending.end());

    for (auto& connection : connections)
    {
        auto e = xml->createNewChildElement("CONNECTION");

        e->setAttribute("srcFilter", (int)connection.source.nodeID.uid);
        e->setAttribute("srcChannel", connection.source.channelIndex);
        e->setAttribute("dstFilter", (int)connection.destination.nodeID.uid);
        e->setAttribute("dstChannel", connection.destination.channelIndex);
    }

    xml->addChildElement(groups.createXml().release());
    if (getTransportProcessor() == nullptr)
    {
        auto* transport = xml->createNewChildElement("TRANSPORT");
        auto savedTransport = getTransportSettings();
        transport->setAttribute("tempoHundredths", savedTransport.tempoHundredths);
        transport->setAttribute("numerator", savedTransport.numerator);
        transport->setAttribute("denominator", savedTransport.denominator);
        transport->setAttribute("fallback", true);
    }
    return xml;
}

Result PluginGraph::restoreFromXml(const XmlElement& xml)
{
    TransportSettings restoredTransport;
    if (auto* transport = xml.getChildByName("TRANSPORT"))
    {
        auto readInteger = [transport](const char* name, int& value)
        {
            String text = transport->getStringAttribute(name);
            value = text.getIntValue();
            return text == String(value);
        };
        if (!readInteger("tempoHundredths", restoredTransport.tempoHundredths)
            || !readInteger("numerator", restoredTransport.numerator)
            || !readInteger("denominator", restoredTransport.denominator)
            || !restoredTransport.isValid())
            return Result::fail("Invalid graph tempo or time signature");
    }
    int chordNodes = 0;
    for (auto* filter : xml.getChildWithTagNameIterator("FILTER"))
        for (auto* child : filter->getChildIterator())
        {
            PluginDescription description;
            if (description.loadFromXml(*child) && ChordAnalyzerPlugin::isDescription(description) && ++chordNodes > 1)
                return Result::fail("Only one Chord Analyzer is allowed per host");
        }
    int transportNodes = 0;
    for (auto* filter : xml.getChildWithTagNameIterator("FILTER"))
        for (auto* child : filter->getChildIterator())
        {
            PluginDescription description;
            if (description.loadFromXml(*child) && TransportProcessor::isTransportDescription(description))
            {
                if (++transportNodes > 1)
                    return Result::fail("Only one Transport is allowed per host");
                if (auto* state = filter->getChildByName("STATE"))
                {
                    MemoryBlock data;
                    TransportSettings settings;
                    int modes = 0;
                    double position = 0.0;
                    if (!data.fromBase64Encoding(state->getAllSubText())
                        || !TransportProcessor::readState(
                            data.getData(),
                            static_cast<int>(data.getSize()),
                            settings,
                            modes,
                            position
                        ))
                        return Result::fail("Invalid Transport state");
                }
            }
        }
    std::set<String> parentRoles;
    for (auto* filter : xml.getChildWithTagNameIterator("FILTER"))
        for (auto* child : filter->getChildIterator())
        {
            PluginDescription description;
            if (description.loadFromXml(*child)
                && atk::ParentSourceProcessor::isParentDescription(description)
                && !parentRoles.insert(description.name).second)
            {
                pluginLoadErrors = {"Duplicate parent source nodes"};
                sendChangeMessage();
                return Result::fail(pluginLoadErrors.joinIntoString("\n"));
            }
        }
    clear(false);
    transportSettings.set(restoredTransport);
    pluginLoadErrors.clear();

    for (auto* e : xml.getChildWithTagNameIterator("FILTER"))
    {
        createNodeFromXml(*e, NodeID((uint32)e->getIntAttribute("uid")), true, &xml);
        changed();
    }

    if (auto* oldTransport = xml.getChildByName("TRANSPORT");
        oldTransport != nullptr && !oldTransport->getBoolAttribute("fallback") && getTransportProcessor() == nullptr)
    {
        auto controller = std::make_unique<TransportProcessor>();
        controller->setSettings(restoredTransport);
        controller->bind(this);
        if (auto node = graph.addNode(std::move(controller), getNextUID()))
        {
            node->properties.set("x", 0.5);
            node->properties.set("y", 0.5);
        }
    }
    ensureParentSourceNodes();

    for (auto* e : xml.getChildWithTagNameIterator("CONNECTION"))
    {
        pendingConnections.addOrDefer(
            graph,
            {
                {NodeID((uint32)e->getIntAttribute("srcFilter")), e->getIntAttribute("srcChannel")},
                {NodeID((uint32)e->getIntAttribute("dstFilter")), e->getIntAttribute("dstChannel")}
        }
        );
    }

    graph.removeIllegalConnections();
    if (auto* savedGroups = xml.getChildByName(NodeGroups::xmlTag))
    {
        groups.restoreFromXml(*savedGroups);
        auto bypass = suspendGroupBypass();
        std::vector<uint32> invalid;
        for (auto& group : groups.getAll())
            for (auto uid : groups.getNodeUids(group.id))
                if (auto* node = graph.getNodeForId(NodeID(uid)); node == nullptr || !isGroupable(*node))
                    invalid.push_back(uid);
        for (auto uid : invalid)
            applyReleasedNodes(groups.removeNode(uid));
        resumeGroupBypass(std::move(bypass));
    }
    syncLastTouchedTracker();
    graph.addChangeListener(this);
    sendChangeMessage();
    return Result::ok();
}

File PluginGraph::getDefaultGraphDocumentOnMobile()
{
    auto persistantStorageLocation = File::getSpecialLocation(File::userApplicationDataDirectory);
    return persistantStorageLocation.getChildFile("state.filtergraph");
}

class PluginGraph::TransportEditAction final : public UndoableAction
{
public:
    TransportEditAction(PluginGraph& graphToEdit, TransportSettings oldSettings, TransportSettings newSettings)
        : owner(graphToEdit)
        , before(oldSettings)
        , after(newSettings)
    {
    }

    bool perform() override
    {
        owner.applyTransportSettings(after);
        return true;
    }

    bool undo() override
    {
        owner.applyTransportSettings(before);
        return true;
    }

    UndoableAction* createCoalescedAction(UndoableAction* nextAction) override
    {
        auto* next = dynamic_cast<TransportEditAction*>(nextAction);
        if (next != nullptr && &owner == &next->owner)
            return new TransportEditAction(owner, before, next->after);
        return nullptr;
    }

private:
    PluginGraph& owner;
    TransportSettings before;
    TransportSettings after;
};

TransportSettings PluginGraph::getTransportSettings() const
{
    return transportSettings.get();
}

PublishedTransport& PluginGraph::getPublishedTransport()
{
    return transportSettings;
}

TransportProcessor* PluginGraph::getTransportProcessor() const
{
    for (auto* node : graph.getNodes())
        if (auto* controller = dynamic_cast<TransportProcessor*>(node->getProcessor()))
            return controller;
    return nullptr;
}

ChordAnalyzerPlugin* PluginGraph::getChordAnalyzer() const
{
    for (auto* node : graph.getNodes())
        if (auto* analyzer = dynamic_cast<ChordAnalyzerPlugin*>(node->getProcessor()))
            return analyzer;
    return nullptr;
}

chord_analyzer::PublishedChord& PluginGraph::getPublishedChord()
{
    return *chordContext;
}

Vst3ChordContext& PluginGraph::getVst3ChordContext()
{
    return *vst3ChordContext;
}

void PluginGraph::syncChordAnalyzer()
{
    AudioProcessorGraphMT::Node::Ptr current;
    std::map<uint32, bool> bypass;
    std::set<Connection> connections;
    for (auto* node : graph.getNodes())
    {
        if (dynamic_cast<ChordAnalyzerPlugin*>(node->getProcessor()) != nullptr)
            current = node;
        if (node->getProcessor()->acceptsMidi() || node->getProcessor()->producesMidi())
            bypass.emplace(node->nodeID.uid, node->isBypassed());
    }
    bool connected = false;
    for (auto& connection : graph.getConnections())
        if (connection.source.channelIndex == AudioProcessorGraphMT::midiChannelIndex)
        {
            connections.insert(connection);
            if (current != nullptr && connection.destination.nodeID == current->nodeID)
                connected = true;
        }
    bool topologyChanged = chordMidiConnections != connections || chordMidiBypass != bypass;
    chordMidiConnections = std::move(connections);
    chordMidiBypass = std::move(bypass);
    if (boundChordNode != current || topologyChanged)
    {
        if (boundChordNode != nullptr)
            static_cast<ChordAnalyzerPlugin*>(boundChordNode->getProcessor())->bind(nullptr, false);
        boundChordNode = current;
    }
    if (current != nullptr)
        static_cast<ChordAnalyzerPlugin*>(current->getProcessor())
            ->bind(chordContext, connected && !current->isBypassed());
}

void PluginGraph::syncTransportNode()
{
    AudioProcessorGraphMT::Node::Ptr current;
    for (auto* node : graph.getNodes())
        if (dynamic_cast<TransportProcessor*>(node->getProcessor()) != nullptr)
        {
            current = node;
            break;
        }
    if (boundTransportNode != current)
    {
        if (boundTransportNode != nullptr)
            static_cast<TransportProcessor*>(boundTransportNode->getProcessor())->bind(nullptr);
        boundTransportNode = current;
    }
    if (current != nullptr)
        static_cast<TransportProcessor*>(current->getProcessor())->bind(this, !current->isBypassed());
}

void PluginGraph::applyTransportSettings(TransportSettings settingsToApply)
{
    transportSettings.set(settingsToApply);
    ++documentRevision;
    changed();
    sendChangeMessage();
}

Result PluginGraph::setTransportSettings(TransportSettings requested, bool continueTapSequence)
{
    if (!requested.isValid())
        return Result::fail("Invalid tempo or time signature");
    auto previous = getTransportSettings();
    if (previous.encode() == requested.encode())
        return Result::ok();
    if (!continueTapSequence || lastTransportEditRevision != getDocumentRevision())
        undoManager.beginNewTransaction("Change tempo or time signature");
    undoManager.perform(new TransportEditAction(*this, previous, requested));
    lastTransportEditRevision = getDocumentRevision();
    return Result::ok();
}

class PluginGraph::EditAction final : public UndoableAction
{
public:
    EditAction(PluginGraph& graphToEdit, Snapshot beforeEdit, Snapshot afterEdit)
        : owner(graphToEdit)
        , before(std::move(beforeEdit))
        , after(std::move(afterEdit))
    {
    }

    bool perform() override
    {
        if (!std::exchange(firstPerform, false))
            owner.applySnapshotChanges(before, after);
        return true;
    }

    bool undo() override
    {
        owner.applySnapshotChanges(after, before);
        return true;
    }

    int getSizeInUnits() override
    {
        return 1 + (int)(before.nodes.size() + after.nodes.size());
    }

private:
    PluginGraph& owner;
    Snapshot before, after;
    bool firstPerform = true;
};

PluginGraph::Edit::Edit(PluginGraph& graphToEdit, const String& name)
    : owner(graphToEdit)
    , generation(graphToEdit.documentGeneration)
{
    owner.beginEdit(name);
}

PluginGraph::Edit::~Edit()
{
    if (generation == owner.documentGeneration)
        owner.endEdit();
}

void PluginGraph::beginEdit(const String& name)
{
    if (activeEdit != nullptr)
        ++activeEdit->depth;
    else
        activeEdit = std::make_unique<ActiveEdit>(ActiveEdit{name, createSnapshot(), 1});
}

void PluginGraph::endEdit()
{
    syncTransportNode();
    syncChordAnalyzer();
    if (activeEdit == nullptr || --activeEdit->depth > 0)
        return;
    auto edit = std::move(activeEdit);
    auto after = createSnapshot();
    bool differs = !edit->before.isEquivalentTo(after);
    if (!differs || resettingDocument || applyingSnapshot)
        return;
    ++documentRevision;
    changed();
    sendChangeMessage();
    undoManager.beginNewTransaction(edit->name);
    undoManager.perform(new EditAction(*this, std::move(edit->before), std::move(after)));
}

bool PluginGraph::Snapshot::isEquivalentTo(const Snapshot& other) const
{
    if (nodes.size() != other.nodes.size() || connections != other.connections)
        return false;
    for (auto& entry : nodes)
    {
        auto found = other.nodes.find(entry.first);
        if (found == other.nodes.end()
            || entry.second.position != found->second.position
            || entry.second.bypassed != found->second.bypassed
            || entry.second.name != found->second.name)
            return false;
    }
    return groups->isEquivalentTo(other.groups.get(), false);
}

PluginGraph::Snapshot PluginGraph::createSnapshot() const
{
    Snapshot snapshot;
    for (auto* node : graph.getNodes())
        snapshot.nodes[node->nodeID.uid] =
            {getNodePosition(node->nodeID), node->isBypassed(), node->properties["customName"].toString(), {}};
    for (auto& connection : graph.getConnections())
        snapshot.connections.insert(connection);
    for (auto& connection : pendingConnections.get())
        snapshot.connections.insert(connection);
    snapshot.groups = groups.createXml();
    return snapshot;
}

void PluginGraph::applySnapshotChanges(Snapshot& from, const Snapshot& to)
{
    ScopedValueSetter<bool> applying(applyingSnapshot, true);
    for (auto& entry : from.nodes)
        if (to.nodes.count(entry.first) == 0)
            if (auto* node = graph.getNodeForId(NodeID(entry.first)))
            {
                entry.second.xml.reset(createNodeXml(node));
                pendingConnections.disconnectNode(node->nodeID);
                graph.removeNode(node->nodeID);
            }
    for (auto& entry : to.nodes)
    {
        auto previous = from.nodes.find(entry.first);
        bool recreated = previous == from.nodes.end();
        auto id = NodeID(entry.first);
        if (recreated && entry.second.xml != nullptr)
            createNodeFromXml(*entry.second.xml, id);
        auto* node = graph.getNodeForId(id);
        if (node == nullptr)
            continue;
        auto& saved = entry.second;
        if (recreated || previous->second.position != saved.position)
        {
            node->properties.set("x", saved.position.x);
            node->properties.set("y", saved.position.y);
        }
        if (recreated || previous->second.bypassed != saved.bypassed)
            node->setBypassed(saved.bypassed);
        if (recreated || previous->second.name != saved.name)
        {
            if (saved.name.isEmpty())
                node->properties.remove("customName");
            else
                node->properties.set("customName", saved.name);
        }
    }
    for (auto& connection : from.connections)
        if (to.connections.count(connection) == 0)
        {
            graph.removeConnection(connection);
            pendingConnections.remove(connection);
        }
    for (auto& connection : to.connections)
        if (from.connections.count(connection) == 0)
            pendingConnections.addOrDefer(graph, connection);
    if (!from.groups->isEquivalentTo(to.groups.get(), false))
        groups.restoreFromXml(*to.groups);
    syncLastTouchedTracker();
    if (nativeEditorHost != nullptr)
        nativeEditorHost->pruneStalePluginWindows(graph);
    ++documentRevision;
    changed();
    sendChangeMessage();
}

Result PluginGraph::setNodeName(NodeID id, const String& name)
{
    if (isParentNode(id))
        return Result::fail("Parent source nodes have fixed names");
    auto* node = graph.getNodeForId(id);
    if (node == nullptr)
        return Result::fail("Node does not exist");
    Edit edit(*this, "Rename node");
    auto normalized = name.trim();
    if (normalized.isEmpty())
        node->properties.remove("customName");
    else
        node->properties.set("customName", normalized);
    return Result::ok();
}

Result PluginGraph::removeNode(NodeID id)
{
    if (isParentNode(id))
        return Result::fail("Parent source nodes are managed by the host");
    auto* node = graph.getNodeForId(id);
    if (node == nullptr)
        return Result::fail("Node does not exist");
    Edit edit(*this, "Delete");
    auto found = activeEdit->before.nodes.find(id.uid);
    if (found != activeEdit->before.nodes.end())
        found->second.xml.reset(createNodeXml(node));
    auto bypass = suspendGroupBypass();
    auto released = groups.removeNode(id.uid);
    pendingConnections.disconnectNode(id);
    graph.removeNode(id);
    applyReleasedNodes(released);
    resumeGroupBypass(std::move(bypass));
    syncLastTouchedTracker();
    if (nativeEditorHost != nullptr)
        nativeEditorHost->pruneStalePluginWindows(graph);
    return Result::ok();
}

Result PluginGraph::setNodeBypassed(NodeID id, bool bypassed)
{
    if (isParentNode(id))
        return Result::fail("Parent source nodes cannot be bypassed");
    if (graph.getNodeForId(id) == nullptr)
        return Result::fail("Node does not exist");
    Edit edit(*this, "Bypass");
    auto state = suspendGroupBypass();
    state.nodeStates[id.uid] = bypassed;
    resumeGroupBypass(std::move(state));
    return Result::ok();
}

Result PluginGraph::addConnection(const Connection& connection)
{
    Edit edit(*this, "Connect");
    if (std::find(pendingConnections.get().begin(), pendingConnections.get().end(), connection)
            != pendingConnections.get().end()
        || !graph.addConnection(connection))
        return Result::fail("Connection is invalid or already exists");
    return Result::ok();
}

Result PluginGraph::restoreConnection(const Connection& connection)
{
    Edit edit(*this, "Restore connection");
    return pendingConnections.addOrDefer(graph, connection) ? Result::ok()
                                                            : Result::fail("Connection cannot be restored");
}

Result PluginGraph::removeConnection(const Connection& connection)
{
    Edit edit(*this, "Disconnect");
    bool removed = graph.removeConnection(connection);
    removed = pendingConnections.remove(connection) || removed;
    return removed ? Result::ok() : Result::fail("Connection does not exist");
}

Result PluginGraph::disconnectNode(NodeID id)
{
    if (graph.getNodeForId(id) == nullptr)
        return Result::fail("Node does not exist");
    Edit edit(*this, "Disconnect");
    graph.disconnectNode(id);
    pendingConnections.disconnectNode(id);
    return Result::ok();
}

bool PluginGraph::isGroupable(const AudioProcessorGraphMT::Node& node)
{
    return dynamic_cast<AudioProcessorGraphMT::AudioGraphIOProcessor*>(node.getProcessor()) == nullptr
        && dynamic_cast<atk::ParentSourceProcessor*>(node.getProcessor()) == nullptr;
}

uint32 PluginGraph::groupNodes(const Array<NodeID>& ids)
{
    Edit edit(*this, "Group");
    std::vector<uint32> uids;
    for (auto id : ids)
        if (auto* node = graph.getNodeForId(id); node != nullptr && isGroupable(*node))
            uids.push_back(id.uid);
    auto children = groups.getGroupIdsWithin(uids);
    auto bypass = suspendGroupBypass();
    auto result = groups.create(uids, children, {});
    applyReleasedNodes(result.released);
    resumeGroupBypass(std::move(bypass));
    if (auto* group = groups.find(result.groupId))
        group->name = "Group " + String(group->id);
    return result.groupId;
}

Result PluginGraph::ungroup(uint32 id)
{
    if (groups.find(id) == nullptr)
        return Result::fail("Group does not exist");
    Edit edit(*this, "Ungroup");
    auto bypass = suspendGroupBypass();
    applyReleasedNodes(groups.ungroup(id));
    resumeGroupBypass(std::move(bypass));
    return Result::ok();
}

Result PluginGraph::renameGroup(uint32 id, const String& name)
{
    auto* group = groups.find(id);
    if (group == nullptr)
        return Result::fail("Group does not exist");
    Edit edit(*this, "Rename group");
    group->name = name;
    return Result::ok();
}

static size_t groupDepth(const NodeGroups& groups, uint32 id)
{
    size_t depth = 0;
    for (auto* parent = groups.findParentOf(id); parent != nullptr; parent = groups.findParentOf(parent->id))
        ++depth;
    return depth;
}

PluginGraph::GroupBypassState PluginGraph::suspendGroupBypass(std::map<uint32, bool> nodeStates)
{
    if (nodeStates.empty())
        for (auto* node : graph.getNodes())
            nodeStates.emplace(node->nodeID.uid, node->isBypassed());
    GroupBypassState state;
    state.nodeStates = std::move(nodeStates);
    for (auto& group : groups.getAll())
        if (group.bypassed)
            state.groupIds.push_back(group.id);
    std::sort(
        state.groupIds.begin(),
        state.groupIds.end(),
        [this](uint32 first, uint32 second) { return groupDepth(groups, first) < groupDepth(groups, second); }
    );
    for (auto id : state.groupIds)
    {
        auto* group = groups.find(id);
        for (auto& saved : group->bypassStates)
            if (auto found = state.nodeStates.find(saved.uid); found != state.nodeStates.end())
                found->second = saved.bypassBeforeGroup;
        if (group->bypassStates.empty())
            for (auto& member : group->members)
                if (auto found = state.nodeStates.find(member.uid); found != state.nodeStates.end())
                    found->second = member.bypassBeforeGroup;
        group->bypassed = false;
        group->bypassStates.clear();
    }
    return state;
}

void PluginGraph::resumeGroupBypass(GroupBypassState state)
{
    auto& ids = state.groupIds;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::sort(
        ids.begin(),
        ids.end(),
        [this](uint32 first, uint32 second) { return groupDepth(groups, first) > groupDepth(groups, second); }
    );
    for (auto id : ids)
    {
        auto* group = groups.find(id);
        if (group == nullptr)
            continue;
        group->bypassStates.clear();
        for (auto uid : groups.getNodeUids(id))
            if (auto found = state.nodeStates.find(uid); found != state.nodeStates.end())
            {
                group->bypassStates.push_back({uid, found->second});
                found->second = true;
            }
        for (auto& member : group->members)
            for (auto& saved : group->bypassStates)
                if (saved.uid == member.uid)
                    member.bypassBeforeGroup = saved.bypassBeforeGroup;
        group->bypassed = true;
    }
    for (auto& entry : state.nodeStates)
        if (auto* node = graph.getNodeForId(NodeID(entry.first)); node != nullptr && node->isBypassed() != entry.second)
            node->setBypassed(entry.second);
}

void PluginGraph::applyReleasedNodes(const std::vector<NodeGroups::ReleasedNode>& released)
{
    for (auto& entry : released)
        if (entry.restoreBypass.has_value())
            if (auto* node = graph.getNodeForId(NodeID(entry.uid)))
                node->setBypassed(*entry.restoreBypass);
}

Result PluginGraph::setGroupBypassed(uint32 id, bool bypassed)
{
    auto* group = groups.find(id);
    if (group == nullptr)
        return Result::fail("Group does not exist");
    if (group->bypassed == bypassed)
        return Result::ok();
    Edit edit(*this, "Group bypass");
    auto state = suspendGroupBypass();
    if (bypassed)
        state.groupIds.push_back(id);
    else
        state.groupIds.erase(std::remove(state.groupIds.begin(), state.groupIds.end(), id), state.groupIds.end());
    resumeGroupBypass(std::move(state));
    return Result::ok();
}

Result PluginGraph::setGroupCollapsed(uint32 id, bool collapsed)
{
    auto* group = groups.find(id);
    if (group == nullptr)
        return Result::fail("Group does not exist");
    Edit edit(*this, collapsed ? "Collapse group" : "Expand group");
    group->collapsed = collapsed;
    return Result::ok();
}

namespace
{
bool readDescription(const XmlElement& xml, PluginDescription& description)
{
    for (auto* child : xml.getChildIterator())
        if (description.loadFromXml(*child))
            return true;
    return false;
}

bool samePlugin(const AudioProcessorGraphMT::Node& node, const XmlElement& xml)
{
    PluginDescription description;
    auto* instance = dynamic_cast<AudioPluginInstance*>(node.getProcessor());
    return instance != nullptr
        && readDescription(xml, description)
        && instance->getPluginDescription().isDuplicateOf(description);
}

bool validState(const XmlElement& xml)
{
    if (auto* saved = xml.getChildByName("STATE"))
    {
        MemoryBlock state;
        if (!state.fromBase64Encoding(saved->getAllSubText()))
            return false;
        PluginDescription description;
        if (readDescription(xml, description) && TransportProcessor::isTransportDescription(description))
        {
            TransportSettings settings;
            int modes = 0;
            double position = 0.0;
            return TransportProcessor::readState(
                state.getData(),
                static_cast<int>(state.getSize()),
                settings,
                modes,
                position
            );
        }
    }
    return true;
}

void applyState(AudioProcessorGraphMT::Node& node, const XmlElement& xml)
{
    if (auto* saved = xml.getChildByName("STATE"))
    {
        MemoryBlock state;
        state.fromBase64Encoding(saved->getAllSubText());
        node.getProcessor()->setStateInformation(state.getData(), (int)state.getSize());
    }
}

Result writeXml(const XmlElement& xml, const File& file)
{
    return xml.writeTo(file, {}) ? Result::ok() : Result::fail("Couldn't write to " + file.getFullPathName());
}

void writeConnection(XmlElement& xml, const PluginGraph::Connection& connection)
{
    auto* child = xml.createNewChildElement("CONNECTION");
    child->setAttribute("srcFilter", (int)connection.source.nodeID.uid);
    child->setAttribute("srcChannel", connection.source.channelIndex);
    child->setAttribute("dstFilter", (int)connection.destination.nodeID.uid);
    child->setAttribute("dstChannel", connection.destination.channelIndex);
}

bool validGroupXml(const XmlElement& xml, NodeGroups& groups)
{
    if (!xml.hasTagName(NodeGroups::xmlTag))
        return false;
    for (auto* child : xml.getChildIterator())
        if (!child->hasTagName("GROUP"))
            return false;
    groups.restoreFromXml(xml);
    std::function<int(const XmlElement&)> countGroups = [&](const XmlElement& parent)
    {
        int count = 0;
        for (auto* child : parent.getChildIterator())
        {
            if (!child->hasTagName("GROUP") && !child->hasTagName("MEMBER") && !child->hasTagName("BYPASS_STATE"))
                return -1000000;
            if (child->hasTagName("GROUP"))
                count += 1 + countGroups(*child);
            else if (!child->hasAttribute("uid") || child->getNumChildElements() != 0)
                return -1000000;
        }
        return count;
    };
    return countGroups(xml) >= 0 && countGroups(xml) == (int)groups.getAll().size();
}
} // namespace

Result PluginGraph::saveNodeState(NodeID id, const File& file) const
{
    auto* node = graph.getNodeForId(id);
    if (node == nullptr)
        return Result::fail("Node not found");
    XmlElement xml("NODESTATE");
    auto* saved = createNodeXml(node);
    if (saved == nullptr)
        return Result::fail("Node state cannot be saved");
    xml.addChildElement(saved);
    return writeXml(xml, file);
}

Result PluginGraph::loadNodeState(NodeID id, const File& file)
{
    auto xml = parseXMLIfTagMatches(file, "NODESTATE");
    auto* saved = xml != nullptr ? xml->getChildByName("FILTER") : nullptr;
    auto* node = graph.getNodeForId(id);
    if (saved == nullptr || !validState(*saved))
        return Result::fail("Not a valid node state file");
    if (node == nullptr || !samePlugin(*node, *saved))
        return Result::fail("The state was saved from a different plugin");
    auto bypass = suspendGroupBypass();
    if (nativeEditorHost != nullptr)
        if (auto* plugin = dynamic_cast<AudioPluginInstance*>(node->getProcessor()))
            nativeEditorHost->bindPluginToOwner(plugin);
    applyState(*node, *saved);
    bypass.nodeStates[id.uid] = saved->getBoolAttribute("bypassed");
    resumeGroupBypass(std::move(bypass));
    ++documentRevision;
    changed();
    sendChangeMessage();
    return Result::ok();
}

Result PluginGraph::saveGroup(uint32 id, const File& file) const
{
    if (groups.find(id) == nullptr)
        return Result::fail("Group not found");
    Array<NodeID> ids;
    for (auto uid : groups.getNodeUids(id))
        ids.add(NodeID(uid));
    return writeXml(*createSnippetXml(ids), file);
}

Result PluginGraph::loadGroupState(uint32 id, const File& file)
{
    if (groups.find(id) == nullptr)
        return Result::fail("Group not found");
    auto xml = parseXMLIfTagMatches(file, "NODES");
    auto* savedXml = xml != nullptr ? xml->getChildByName(NodeGroups::xmlTag) : nullptr;
    NodeGroups savedGroups;
    if (savedXml == nullptr || !validGroupXml(*savedXml, savedGroups) || savedXml->getNumChildElements() != 1)
        return Result::fail("Not a valid node group tree");
    auto savedId = (uint32)savedXml->getChildElement(0)->getIntAttribute("id");
    if (savedGroups.find(savedId) == nullptr)
        return Result::fail("Not a valid node group tree");
    std::map<uint32, const XmlElement*> savedNodes;
    for (auto* child : xml->getChildWithTagNameIterator("FILTER"))
        if (!savedNodes.emplace((uint32)child->getIntAttribute("uid"), child).second || !validState(*child))
            return Result::fail("Invalid or duplicate saved node");
    if (savedNodes.size() != groups.getNodeUids(id).size()
        || savedNodes.size() != savedGroups.getNodeUids(savedId).size())
        return Result::fail("The saved nodes don't match the group");
    std::map<uint32, uint32> uidMap;
    std::vector<std::pair<uint32, uint32>> groupMap;
    std::function<bool(uint32, uint32)> matchTree = [&](uint32 sourceId, uint32 targetId)
    {
        auto* source = savedGroups.find(sourceId);
        auto* target = groups.find(targetId);
        if (source == nullptr
            || target == nullptr
            || source->members.size() != target->members.size()
            || source->childGroups.size() != target->childGroups.size())
            return false;
        groupMap.emplace_back(sourceId, targetId);
        for (size_t index = 0; index < source->members.size(); ++index)
            uidMap.emplace(source->members[index].uid, target->members[index].uid);
        for (size_t index = 0; index < source->childGroups.size(); ++index)
            if (!matchTree(source->childGroups[index].groupId, target->childGroups[index].groupId))
                return false;
        return true;
    };
    if (!matchTree(savedId, id))
        return Result::fail("The saved group nesting doesn't match");
    for (auto& entry : uidMap)
    {
        auto found = savedNodes.find(entry.first);
        auto* node = graph.getNodeForId(NodeID(entry.second));
        if (found == savedNodes.end() || node == nullptr || !samePlugin(*node, *found->second))
            return Result::fail("A saved plugin doesn't match the group");
    }
    auto bypass = suspendGroupBypass();
    for (auto& entry : uidMap)
    {
        auto* node = graph.getNodeForId(NodeID(entry.second));
        if (nativeEditorHost != nullptr)
            if (auto* plugin = dynamic_cast<AudioPluginInstance*>(node->getProcessor()))
                nativeEditorHost->bindPluginToOwner(plugin);
        applyState(*node, *savedNodes.at(entry.first));
        bypass.nodeStates[entry.second] = savedNodes.at(entry.first)->getBoolAttribute("bypassed");
    }
    for (auto& entry : groupMap)
    {
        auto* saved = savedGroups.find(entry.first);
        auto* target = groups.find(entry.second);
        target->bypassed = saved->bypassed;
        target->bypassStates = saved->bypassStates;
        for (auto& state : target->bypassStates)
            state.uid = uidMap.at(state.uid);
        for (size_t index = 0; index < target->members.size(); ++index)
            target->members[index].bypassBeforeGroup = saved->members[index].bypassBeforeGroup;
        bypass.groupIds.erase(
            std::remove(bypass.groupIds.begin(), bypass.groupIds.end(), entry.second),
            bypass.groupIds.end()
        );
    }
    auto loaded = suspendGroupBypass(std::move(bypass.nodeStates));
    bypass.groupIds.insert(bypass.groupIds.end(), loaded.groupIds.begin(), loaded.groupIds.end());
    bypass.nodeStates = std::move(loaded.nodeStates);
    resumeGroupBypass(std::move(bypass));
    ++documentRevision;
    changed();
    sendChangeMessage();
    return Result::ok();
}

Result PluginGraph::insertGroupFromFile(const File& file, Point<double> position)
{
    auto xml = parseXMLIfTagMatches(file, "NODES");
    if (xml == nullptr)
        return Result::fail("Not a valid node group file");
    Edit edit(*this, "Insert group");
    return insertSnippetXml(*xml, position);
}

bool PluginGraph::isSnippetXml(const XmlElement& xml)
{
    return xml.hasTagName("NODES");
}

std::unique_ptr<XmlElement> PluginGraph::createSnippetXml(const Array<NodeID>& ids) const
{
    std::vector<uint32> uids;
    Point<double> topLeft;
    for (auto id : ids)
        if (auto* node = graph.getNodeForId(id);
            node != nullptr && isGroupable(*node) && std::find(uids.begin(), uids.end(), id.uid) == uids.end())
        {
            uids.push_back(id.uid);
            auto position = getNodePosition(id);
            topLeft =
                uids.size() == 1 ? position : Point<double>(jmin(topLeft.x, position.x), jmin(topLeft.y, position.y));
        }
    auto xml = std::make_unique<XmlElement>("NODES");
    xml->setAttribute("x", topLeft.x);
    xml->setAttribute("y", topLeft.y);
    for (auto uid : uids)
    {
        auto* node = graph.getNodeForId(NodeID(uid));
        auto* saved = createNodeXml(node);
        if (saved == nullptr)
            continue;
        for (auto* group = groups.findGroupOf(uid); group != nullptr; group = groups.findParentOf(group->id))
        {
            auto descendants = groups.getNodeUids(group->id);
            bool included = std::all_of(
                descendants.begin(),
                descendants.end(),
                [&uids](uint32 descendant) { return std::find(uids.begin(), uids.end(), descendant) != uids.end(); }
            );
            if (!included && group->bypassed)
            {
                for (auto& state : group->bypassStates)
                    if (state.uid == uid)
                        saved->setAttribute("bypassed", state.bypassBeforeGroup);
                if (group->bypassStates.empty())
                    for (auto& member : group->members)
                        if (member.uid == uid)
                            saved->setAttribute("bypassed", member.bypassBeforeGroup);
            }
        }
        auto position = getNodePosition(node->nodeID) - topLeft;
        saved->setAttribute("x", position.x);
        saved->setAttribute("y", position.y);
        xml->addChildElement(saved);
    }
    std::set<Connection> connections;
    for (auto& connection : graph.getConnections())
        connections.insert(connection);
    for (auto& connection : pendingConnections.get())
        connections.insert(connection);
    for (auto& connection : connections)
        if (std::find(uids.begin(), uids.end(), connection.source.nodeID.uid) != uids.end()
            && std::find(uids.begin(), uids.end(), connection.destination.nodeID.uid) != uids.end())
            writeConnection(*xml, connection);
    xml->addChildElement(groups.createXmlForGroupsWithin(uids).release());
    return xml;
}

Result PluginGraph::insertSnippetXml(const XmlElement& xml, Point<double> position, Array<NodeID>* inserted)
{
    if (inserted != nullptr)
        inserted->clear();
    if (!isSnippetXml(xml))
        return Result::fail("Not a valid node snippet");
    std::set<uint32> sourceIds;
    int transportNodes = getTransportProcessor() != nullptr ? 1 : 0;
    int chordNodes = getChordAnalyzer() != nullptr ? 1 : 0;
    int groupSections = 0;
    for (auto* child : xml.getChildIterator())
    {
        if (!child->hasTagName("FILTER") && !child->hasTagName("CONNECTION") && !child->hasTagName(NodeGroups::xmlTag))
            return Result::fail("Unexpected snippet content");
        if (child->hasTagName(NodeGroups::xmlTag) && ++groupSections > 1)
            return Result::fail("Duplicate group sections");
        if (!child->hasTagName("FILTER"))
            continue;
        auto uid = (uint32)child->getIntAttribute("uid");
        PluginDescription description;
        if (uid == 0 || !sourceIds.insert(uid).second || !readDescription(*child, description) || !validState(*child))
            return Result::fail("Invalid or duplicate snippet node");
        if (atk::ParentSourceProcessor::isParentDescription(description))
            return Result::fail("Parent source nodes cannot be pasted");
        if (TransportProcessor::isTransportDescription(description) && ++transportNodes > 1)
            return Result::fail("Only one Transport is allowed per host");
        if (ChordAnalyzerPlugin::isDescription(description) && ++chordNodes > 1)
            return Result::fail("Only one Chord Analyzer is allowed per host");
    }
    if (sourceIds.empty())
        return Result::fail("No nodes found");
    NodeGroups importedGroups;
    auto* groupsXml = xml.getChildByName(NodeGroups::xmlTag);
    if (groupsXml != nullptr)
    {
        if (!validGroupXml(*groupsXml, importedGroups))
            return Result::fail("Invalid group tree");
        for (auto& group : importedGroups.getAll())
            for (auto& member : group.members)
                if (sourceIds.count(member.uid) == 0)
                    return Result::fail("Group references a missing node");
    }
    std::set<Connection> sourceRoutes;
    for (auto* child : xml.getChildWithTagNameIterator("CONNECTION"))
    {
        if (sourceIds.count((uint32)child->getIntAttribute("srcFilter")) == 0
            || sourceIds.count((uint32)child->getIntAttribute("dstFilter")) == 0
            || !child->hasAttribute("srcChannel")
            || !child->hasAttribute("dstChannel"))
            return Result::fail("Invalid snippet route");
        Connection route{
            {NodeID((uint32)child->getIntAttribute("srcFilter")), child->getIntAttribute("srcChannel")},
            {NodeID((uint32)child->getIntAttribute("dstFilter")), child->getIntAttribute("dstChannel")}
        };
        if (!sourceRoutes.insert(route).second)
            return Result::fail("Duplicate snippet route");
    }
    Edit edit(*this, "Paste");
    pluginLoadErrors.clear();
    Array<NodeID> created;
    std::map<uint32, uint32> uidMap;
    auto rollback = [&]()
    {
        for (auto id : created)
        {
            pendingConnections.disconnectNode(id);
            graph.removeNode(id);
        }
        syncLastTouchedTracker();
    };
    for (auto* child : xml.getChildWithTagNameIterator("FILTER"))
    {
        XmlElement placed(*child);
        placed.setAttribute("x", position.x + child->getDoubleAttribute("x"));
        placed.setAttribute("y", position.y + child->getDoubleAttribute("y"));
        for (int index = 0; index < (int)PluginWindow::Type::numTypes; ++index)
            placed.setAttribute(PluginWindow::getOpenProp((PluginWindow::Type)index), false);
        auto node = createNodeFromXml(placed, {}, false, &xml);
        if (node == nullptr)
        {
            rollback();
            return Result::fail("Couldn't insert plugin: " + pluginLoadErrors.joinIntoString(", "));
        }
        created.add(node->nodeID);
        if (!isGroupable(*node))
        {
            rollback();
            return Result::fail("Hardware I/O nodes cannot be pasted");
        }
        uidMap.emplace((uint32)child->getIntAttribute("uid"), node->nodeID.uid);
    }
    for (auto* child : xml.getChildWithTagNameIterator("CONNECTION"))
    {
        Connection route{
            {NodeID(uidMap.at((uint32)child->getIntAttribute("srcFilter"))), child->getIntAttribute("srcChannel")},
            {NodeID(uidMap.at((uint32)child->getIntAttribute("dstFilter"))), child->getIntAttribute("dstChannel")}
        };
        if (!pendingConnections.addOrDefer(graph, route))
        {
            rollback();
            return Result::fail("Couldn't insert invalid snippet route");
        }
    }
    if (groupsXml != nullptr)
        groups.addFromXml(*groupsXml, uidMap);
    syncLastTouchedTracker();
    if (inserted != nullptr)
        *inserted = created;
    return Result::ok();
}
