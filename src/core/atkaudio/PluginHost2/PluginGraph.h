#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

#include "Editor/PluginWindow.h"
#include "../AudioProcessorGraphMT/AudioProcessorGraphMT.h"
#include "PendingConnections.h"
#include "NodeGroups.h"
#include "NativeEditorHost.h"
#include "ParentSourceBridge.h"
#include "HostTransport.h"
#include "InternalPlugins/ChordAnalyzer.h"
#include "Vst3ChordContext.h"

#include <map>
#include <set>
#include <unordered_map>

// Using AudioProcessorGraphMT from atk namespace
using atk::AudioProcessorGraphMT;

struct PluginDescriptionAndPreference
{
    enum class UseARA
    {
        no,
        yes
    };

    PluginDescriptionAndPreference() = default;

    explicit PluginDescriptionAndPreference(PluginDescription pd)
        : pluginDescription(std::move(pd))
        , useARA(
              pluginDescription.hasARAExtension ? PluginDescriptionAndPreference::UseARA::yes
                                                : PluginDescriptionAndPreference::UseARA::no
          )
    {
    }

    PluginDescriptionAndPreference(PluginDescription pd, UseARA ara)
        : pluginDescription(std::move(pd))
        , useARA(ara)
    {
    }

    PluginDescription pluginDescription;
    UseARA useARA = UseARA::no;
};

class PluginGraph final
    : public FileBasedDocument
    , public AudioProcessorListener
    , private ChangeListener
{
public:
    PluginGraph(MainHostWindow&, AudioPluginFormatManager&, KnownPluginList&);
    PluginGraph(AudioPluginFormatManager&, KnownPluginList&, PropertiesFile* = nullptr, NativeEditorHost* = nullptr);
    ~PluginGraph() override;

    using NodeID = AudioProcessorGraphMT::NodeID;
    using Connection = AudioProcessorGraphMT::Connection;

    class Edit
    {
    public:
        Edit(PluginGraph&, const String&);
        ~Edit();

    private:
        PluginGraph& owner;
        uint64 generation;
        JUCE_DECLARE_NON_COPYABLE(Edit)
    };

    UndoManager& getUndoManager()
    {
        return undoManager;
    }

    bool isEditInProgress() const
    {
        return activeEdit != nullptr;
    }

    uint64 getDocumentRevision() const
    {
        return documentRevision.load(std::memory_order_acquire);
    }

    uint64 getDocumentGeneration() const
    {
        return documentGeneration;
    }

    NativeEditorHost* getNativeEditorHost() const
    {
        return nativeEditorHost;
    }

    using PluginAddedCallback = std::function<void(const Result&, NodeID)>;
    void addPlugin(const PluginDescriptionAndPreference&, Point<double>, PluginAddedCallback = {});

    AudioProcessorGraphMT::Node::Ptr getNodeForName(const String& name) const;

    Result setNodeName(NodeID, const String&);
    Result setNodePosition(NodeID, Point<double>);
    Point<double> getNodePosition(NodeID) const;
    Result removeNode(NodeID);
    Result setNodeBypassed(NodeID, bool);
    Result addConnection(const Connection&);
    Result restoreConnection(const Connection&);
    Result removeConnection(const Connection&);
    Result disconnectNode(NodeID);
    static bool isGroupable(const AudioProcessorGraphMT::Node&);
    uint32 groupNodes(const Array<NodeID>&);
    Result ungroup(uint32);
    Result renameGroup(uint32, const String&);
    Result setGroupBypassed(uint32, bool);
    Result setGroupCollapsed(uint32, bool);

    const NodeGroups& getGroups() const
    {
        return groups;
    }

    Result saveNodeState(NodeID, const File&) const;
    Result loadNodeState(NodeID, const File&);
    Result saveGroup(uint32, const File&) const;
    Result loadGroupState(uint32, const File&);
    Result insertGroupFromFile(const File&, Point<double>);
    std::unique_ptr<XmlElement> createSnippetXml(const Array<NodeID>&) const;
    Result insertSnippetXml(const XmlElement&, Point<double>, Array<NodeID>* = nullptr);
    static bool isSnippetXml(const XmlElement&);
    static constexpr const char* nodeStateWildcard = "*.nodestate";
    static constexpr const char* nodeGroupWildcard = "*.nodegroup";

    const StringArray& getPluginLoadErrors() const
    {
        return pluginLoadErrors;
    }

    void clear(bool resetTransport = true);
    TransportSettings getTransportSettings() const;
    PublishedTransport& getPublishedTransport();
    class TransportProcessor* getTransportProcessor() const;
    class ChordAnalyzerPlugin* getChordAnalyzer() const;
    chord_analyzer::PublishedChord& getPublishedChord();
    Vst3ChordContext& getVst3ChordContext();
    Result setTransportSettings(TransportSettings, bool continueTapSequence = false);
    void refreshLastTouchedTrackerOwnership();
    void setParentSourceBridge(atk::ParentSourceBridge*);
    bool isNodeVisible(NodeID) const;
    bool isParentNode(NodeID) const;

    PluginWindow* getOrCreateWindowFor(AudioProcessorGraphMT::Node*, PluginWindow::Type);
    bool closeAnyOpenPluginWindows();

    void audioProcessorParameterChanged(AudioProcessor*, int, float) override
    {
    }

    void audioProcessorChanged(AudioProcessor*, const ChangeDetails&) override
    {
        changed();
    }

    std::unique_ptr<XmlElement> createXml() const;
    Result restoreFromXml(const XmlElement&);

    static const char* getFilenameSuffix()
    {
        return ".filtergraph";
    }

    static const char* getFilenameWildcard()
    {
        return "*.filtergraph";
    }

    void newDocument();
    String getDocumentTitle() override;
    Result loadDocument(const File& file) override;
    Result saveDocument(const File& file) override;
    File getLastDocumentOpened() override;
    void setLastDocumentOpened(const File& file) override;

    static File getDefaultGraphDocumentOnMobile();

    static File getLastSessionGraphFile()
    {
        return getDefaultGraphDocumentOnMobile();
    }

    AudioProcessorGraphMT graph;

    static std::atomic<int> activeInstanceCount;

    static bool hasActiveInstances()
    {
        return activeInstanceCount.load() > 0;
    }

private:
    NativeEditorHost* nativeEditorHost = nullptr;
    AudioPluginFormatManager& formatManager;
    KnownPluginList& knownPlugins;
    PropertiesFile* settings = nullptr;
    ScopedMessageBox messageBox;
    std::shared_ptr<std::atomic<bool>> lifetimeToken = std::make_shared<std::atomic<bool>>(true);
    uint64 documentGeneration = 0;
    std::atomic<uint64> documentRevision{0};
    bool resettingDocument = false;
    bool applyingSnapshot = false;
    NodeGroups groups;
    StringArray pluginLoadErrors;
    PublishedTransport transportSettings;
    AudioProcessorGraphMT::Node::Ptr boundTransportNode;
    void syncTransportNode();
    std::shared_ptr<chord_analyzer::PublishedChord> chordContext = std::make_shared<chord_analyzer::PublishedChord>();
    std::shared_ptr<Vst3ChordContext> vst3ChordContext = std::make_shared<Vst3ChordContext>(chordContext);
    AudioProcessorGraphMT::Node::Ptr boundChordNode;
    std::set<Connection> chordMidiConnections;
    std::map<uint32, bool> chordMidiBypass;
    void syncChordAnalyzer();
    uint64 lastTransportEditRevision = 0;
    class TransportEditAction;
    void applyTransportSettings(TransportSettings);

    struct Snapshot
    {
        struct Node
        {
            Point<double> position;
            bool bypassed = false;
            String name;
            std::shared_ptr<XmlElement> xml;
        };

        std::map<uint32, Node> nodes;
        std::set<Connection> connections;
        std::shared_ptr<XmlElement> groups;
        bool isEquivalentTo(const Snapshot&) const;
    };

    struct ActiveEdit
    {
        String name;
        Snapshot before;
        int depth = 1;
    };
    class EditAction;
    UndoManager undoManager;
    std::unique_ptr<ActiveEdit> activeEdit;
    void beginEdit(const String&);
    void endEdit();
    Snapshot createSnapshot() const;
    void applySnapshotChanges(Snapshot&, const Snapshot&);

    struct GroupBypassState
    {
        std::vector<uint32> groupIds;
        std::map<uint32, bool> nodeStates;
    };

    GroupBypassState suspendGroupBypass(std::map<uint32, bool> = {});
    void resumeGroupBypass(GroupBypassState);
    void applyReleasedNodes(const std::vector<NodeGroups::ReleasedNode>&);

    NodeID lastUID;
    NodeID getNextUID() noexcept;

    AudioProcessorGraphMT::Node::Ptr createNodeFromXml(
        const XmlElement&,
        NodeID = {},
        bool restoreWindows = true,
        const XmlElement* sourceGraph = nullptr
    );
    void syncLastTouchedTracker();
    void addPluginCallback(
        std::unique_ptr<AudioPluginInstance>,
        const String& error,
        Point<double>,
        PluginDescriptionAndPreference::UseARA useARA,
        const PluginAddedCallback&
    );
    void changeListenerCallback(ChangeBroadcaster*) override;

    // Holding the node keeps its processor alive until it has been unregistered from the tracker,
    // since nodes may be removed from the graph before the change notification arrives.
    std::unordered_map<uint32_t, AudioProcessorGraphMT::Node::Ptr> trackedNodeProcessors;
    juce::String trackedOwnerSourceUuid;
    juce::String trackedOwnerFilterName;

    PendingConnections pendingConnections;
    atk::ParentSourceBridge* parentSourceBridge = nullptr;
    void ensureParentSourceNodes();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginGraph)
};
