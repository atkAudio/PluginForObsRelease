#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

#include "../UI/PluginWindow.h"

//==============================================================================
/** A type that encapsulates a PluginDescription and some preferences regarding
    how plugins of that description should be instantiated.
*/
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

//==============================================================================
/**
    A collection of plugins and some connections between them.
*/
class PluginGraph final
    : public FileBasedDocument
    , public AudioProcessorListener
    , private ChangeListener
{
public:
    //==============================================================================
    PluginGraph(MainHostWindow&, AudioPluginFormatManager&, KnownPluginList&);
    ~PluginGraph() override;

    //==============================================================================
    using NodeID = AudioProcessorGraph::NodeID;

    void addPlugin(const PluginDescriptionAndPreference&, Point<double>);

    AudioProcessorGraph::Node::Ptr getNodeForName(const String& name) const;

    void setNodePosition(NodeID, Point<double>);
    Point<double> getNodePosition(NodeID) const;

    //==============================================================================
    void clear();

    PluginWindow* getOrCreateWindowFor(AudioProcessorGraph::Node*, PluginWindow::Type);
    void closeCurrentlyOpenWindowsFor(AudioProcessorGraph::NodeID);
    bool closeAnyOpenPluginWindows();

    //==============================================================================
    void audioProcessorParameterChanged(AudioProcessor*, int, float) override
    {
    }

    void audioProcessorChanged(AudioProcessor*, const ChangeDetails&) override
    {
        changed();
    }

    //==============================================================================
    std::unique_ptr<XmlElement> createXml() const;
    void restoreFromXml(const XmlElement&);

    static const char* getFilenameSuffix()
    {
        return ".filtergraph";
    }

    static const char* getFilenameWildcard()
    {
        return "*.filtergraph";
    }

    //==============================================================================
    void newDocument();
    String getDocumentTitle() override;
    Result loadDocument(const File& file) override;
    Result saveDocument(const File& file) override;
    File getLastDocumentOpened() override;
    void setLastDocumentOpened(const File& file) override;

    static File getDefaultGraphDocumentOnMobile();

    //==============================================================================
    AudioProcessorGraph graph;

private:
    //==============================================================================
    MainHostWindow& mainHostWindow;
    AudioPluginFormatManager& formatManager;
    KnownPluginList& knownPlugins;
    OwnedArray<PluginWindow> activePluginWindows;
    ScopedMessageBox messageBox;

    NodeID lastUID;
    NodeID getNextUID() noexcept;

    void createNodeFromXml(const XmlElement&);
    void addPluginCallback(
        std::unique_ptr<AudioPluginInstance>,
        const String& error,
        Point<double>,
        PluginDescriptionAndPreference::UseARA useARA
    );
    void changeListenerCallback(ChangeBroadcaster*) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginGraph)
};
