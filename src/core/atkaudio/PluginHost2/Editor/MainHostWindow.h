#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

#include "../../LookAndFeel.h"
#include "../../SharedPluginList.h"

#include "../PluginGraph.h"
#include "../PluginHost2Settings.h"
#include "GraphEditorPanel.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <functional>
#include <string>

class MainHostWindow final
    : public juce::DocumentWindow
    , public MenuBarModel
    , public ApplicationCommandTarget
    , public ChangeListener
    , public FileDragAndDropTarget
    , public NativeEditorHost
{
public:
    explicit MainHostWindow(juce::AudioDeviceManager& deviceManagerIn);
    ~MainHostWindow() override;

    void attachGraph(PluginGraph& graphModel);
    void detachGraphEditor();

    void closeButtonPressed() override;
    void moved() override;
    void resized() override;
    void changeListenerCallback(ChangeBroadcaster*) override;

    bool isInterestedInFileDrag(const StringArray& files) override;
    void fileDragEnter(const StringArray& files, int, int) override;
    void fileDragMove(const StringArray& files, int, int) override;
    void fileDragExit(const StringArray& files) override;
    void filesDropped(const StringArray& files, int, int) override;

    void menuBarActivated(bool isActive) override;

    StringArray getMenuBarNames() override;
    PopupMenu getMenuForIndex(int topLevelMenuIndex, const String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;
    ApplicationCommandTarget* getNextCommandTarget() override;
    void getAllCommands(Array<CommandID>&) override;
    void getCommandInfo(CommandID, ApplicationCommandInfo&) override;
    bool perform(const InvocationInfo&) override;

    void tryToQuitApplication();

    void createPlugin(const PluginDescriptionAndPreference&, Point<int> pos);
    void createPluginFromMenu(int, Point<double>) override;

    PluginWindow* getOrCreatePluginWindowFor(AudioProcessorGraphMT::Node*, PluginWindow::Type);
    bool closeAnyOpenPluginWindows();
    void pruneStalePluginWindows(const AudioProcessorGraphMT& graphModel);
    void bindPluginToOwner(AudioPluginInstance*) override;

    void addPluginsToMenu(PopupMenu&);
    std::optional<PluginDescriptionAndPreference> getChosenType(int menuID) const;

    // Parent OBS source UUID (the source in which this PluginHost2 filter is)
    void setParentSourceUuid(const std::string& uuid)
    {
        parentSourceUuid = uuid;
    }

    const std::string& getParentSourceUuid() const
    {
        return parentSourceUuid;
    }

    void setOwnerFilterName(const std::string& name)
    {
        ownerFilterName = name;
    }

    const std::string& getOwnerFilterName() const
    {
        return ownerFilterName;
    }

    std::unique_ptr<GraphDocumentComponent> graphHolder;

    ApplicationCommandManager commandManager;

    auto& getAppProperties()
    {
        return *sharedSettings->properties;
    }

    ApplicationCommandManager& getCommandManager() override
    {
        return commandManager;
    }

    void getGraphXml(XmlElement& xml)
    {
        if (graphHolder != nullptr)
            xml.operator=(*graphHolder->graph->createXml());
    }

    void setGraphXml(const XmlElement& xml) const
    {
        if (graphHolder != nullptr)
            graphHolder->graph->restoreFromXml(xml);
    }

    auto& getAudioServer()
    {
        return *audioServer;
    }

    auto& getMidiServer()
    {
        return *midiServer;
    }

    auto& getMidiClient()
    {
        // Always use external MidiClient from ModuleDeviceManager
        jassert(externalMidiClient != nullptr);
        return *externalMidiClient;
    }

    // Set the external MIDI client from ModuleDeviceManager (required)
    void setExternalMidiClient(atk::MidiClient& external)
    {
        externalMidiClient = &external;
    }

    AudioDeviceManager& getDeviceManager() override
    {
        return deviceManager;
    }

    auto& getFormatManager()
    {
        return formatManager;
    }

    auto& getKnownPluginList()
    {
        return knownPluginList;
    }

    void setRuntimeCpuLoadProvider(std::function<float()> provider)
    {
        runtimeCpuLoadProvider = std::move(provider);
    }

    float getRuntimeCpuLoad() const
    {
        return runtimeCpuLoadProvider ? runtimeCpuLoadProvider() : 0.0f;
    }

private:
    void showAudioSettings();
    void showMidiSettings();

    juce::SharedResourcePointer<PluginHost2Settings> sharedSettings;
    bool initialisingWindow = true;

    juce::DialogWindow* audioSettingsDialogWindow = nullptr;
    juce::DialogWindow* midiSettingsDialogWindow = nullptr;

    // NEW: Use AudioServer and MidiServer instead of AudioDeviceManager
    atk::AudioServer* audioServer = nullptr;
    atk::MidiServer* midiServer = nullptr;

    // NEW: MidiClient from ModuleDeviceManager (external reference, required)
    atk::MidiClient* externalMidiClient = nullptr;

    // Audio device manager is owned by PluginHost2 processor-side runtime.
    AudioDeviceManager& deviceManager;

    AudioPluginFormatManager formatManager;

    std::vector<PluginDescription> internalTypes;

    OwnedArray<PluginWindow> activePluginWindows;

    // Own plugin list instance, loads from/saves to shared file
    KnownPluginList knownPluginList;

    KnownPluginList::SortMethod pluginSortMethod = KnownPluginList::sortByManufacturer;
    Array<PluginDescriptionAndPreference> pluginDescriptionsAndPreference;

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;

    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    // Parent OBS source UUID (not ref-counted, just the UUID string)
    std::string parentSourceUuid;
    std::string ownerFilterName;

    std::function<float()> runtimeCpuLoadProvider;

    JUCE_DECLARE_NON_COPYABLE(MainHostWindow)
};
