#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

#include "../../LookAndFeel.h"
#include "../../SharedPluginList.h"

#include "../Core/PluginGraph.h"
#include "GraphEditorPanel.h"

#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <obs-module.h>
#include <string>

namespace CommandIDs
{
#if !(JUCE_IOS || JUCE_ANDROID)
static const int open = 0x30000;
static const int save = 0x30001;
static const int saveAs = 0x30002;
static const int newFile = 0x30003;
#endif
static const int showPluginListEditor = 0x30100;
static const int showAudioSettings = 0x30200;
static const int showMidiSettings = 0x30210;
static const int aboutBox = 0x30300;
static const int allWindowsForward = 0x30400;
static const int autoScalePluginWindows = 0x30600;
} // namespace CommandIDs

enum class AutoScale
{
    scaled,
    unscaled,
    useDefault
};

constexpr bool autoScaleOptionAvailable = false;

// AutoScale getAutoScaleValueForPlugin(const String&);
// void setAutoScaleValueForPlugin(const String&, AutoScale);
// bool shouldAutoScalePlugin(const PluginDescription&);
// void addPluginAutoScaleOptionsSubMenu(AudioPluginInstance*, PopupMenu&);

constexpr const char* processUID = "atkAudioPluginHost2";

class MainHostWindow final
    : public juce::DocumentWindow
    , public MenuBarModel
    , public ApplicationCommandTarget
    , public ChangeListener
    , public FileDragAndDropTarget
{
public:
    MainHostWindow();
    ~MainHostWindow() override;

    void closeButtonPressed() override;
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

    std::unique_ptr<GraphDocumentComponent> graphHolder;

    ApplicationCommandManager commandManager;
    std::unique_ptr<ApplicationProperties> appProperties;

    auto& getAppProperties()
    {
        return *appProperties;
    }

    auto& getCommandManager()
    {
        return commandManager;
    }

    void initialise(const String& commandLine)
    {
        (void)commandLine;

        // initialise our settings file..

        PropertiesFile::Options options;
        options.applicationName = "atkAudio PluginHost2";
        options.filenameSuffix = "settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = "atkAudio Plugin";
        options.processLock = &interprocessLock;

        appProperties.reset(new ApplicationProperties());
        appProperties->setStorageParameters(options);

        commandManager.registerAllCommandsForTarget(this);

        this->menuItemsChanged();
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

    auto& getDeviceManager()
    {
        return deviceManager;
    }

private:
    bool isAutoScalePluginWindowsEnabled();

    void updateAutoScaleMenuItem(ApplicationCommandInfo& info);

    void showAudioSettings();
    void showMidiSettings();

    static inline juce::InterProcessLock interprocessLock{"atkAudioPluginHost2Lock"};

    juce::DialogWindow* audioSettingsDialogWindow = nullptr;
    juce::DialogWindow* midiSettingsDialogWindow = nullptr;

    // NEW: Use AudioServer and MidiServer instead of AudioDeviceManager
    atk::AudioServer* audioServer = nullptr;
    atk::MidiServer* midiServer = nullptr;

    // NEW: MidiClient from ModuleDeviceManager (external reference, required)
    atk::MidiClient* externalMidiClient = nullptr;

    // OLD: Keep for backward compatibility with VirtualAudioIODevice
    AudioDeviceManager deviceManager;
    juce::String lastDeviceName; // Track device changes

    AudioPluginFormatManager formatManager;

    std::vector<PluginDescription> internalTypes;

    // Own plugin list instance, loads from/saves to shared file
    KnownPluginList knownPluginList;

    KnownPluginList::SortMethod pluginSortMethod = KnownPluginList::sortByManufacturer;
    Array<PluginDescriptionAndPreference> pluginDescriptionsAndPreference;

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;

    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    // Parent OBS source UUID (not ref-counted, just the UUID string)
    std::string parentSourceUuid;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainHostWindow)
};
