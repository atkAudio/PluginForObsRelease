#include "MainHostWindow.h"

#include "../../About.h"
#include "../../SandboxedPluginScanner.h"
#include "../../SharedPluginList.h"
#include "../Core/InternalPlugins.h"

#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServerSettingsComponent.h>

class MainHostWindow::PluginListWindow final : public juce::DocumentWindow
{
public:
    PluginListWindow(MainHostWindow& mw, AudioPluginFormatManager& pluginFormatManager)
        : juce::DocumentWindow(
              "Available Plugins",
              LookAndFeel::getDefaultLookAndFeel().findColour(ResizableWindow::backgroundColourId),
              DocumentWindow::allButtons
          )
        , owner(mw)
    {
        setTitleBarButtonsRequired(DocumentWindow::minimiseButton | DocumentWindow::closeButton, false);

        auto* sharedProps = atk::SharedPluginList::getInstance()->getPropertiesFile();
        auto deadMansPedalFile = sharedProps->getFile().getSiblingFile("RecentlyCrashedPluginsList");

        auto* pluginListComponent = new PluginListComponent(
            pluginFormatManager,
            owner.knownPluginList,
            deadMansPedalFile,
            sharedProps,
            false // sync scan
        );

        // Use sandboxed scanner (shows warning once if not available)
        auto sandboxedScanner = std::make_unique<atk::SandboxedScanner>();
        sandboxedScanner->setFormatManager(&pluginFormatManager);
        sandboxedScanner->setKnownPluginList(&owner.knownPluginList);
        if (sandboxedScanner->isScannerAvailable())
            DBG("PluginListWindow: Using sandboxed plugin scanner");
        else
            DBG("PluginListWindow: Sandboxed scanner not available, using in-process scanning");
        owner.knownPluginList.setCustomScanner(std::move(sandboxedScanner));

        setContentOwned(pluginListComponent, true);

        setResizable(true, false);
        setResizeLimits(300, 400, 800, 1500);
        setTopLeftPosition(60, 60);

        setVisible(true);
    }

    ~PluginListWindow() override
    {
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        owner.pluginListWindow = nullptr;
    }

private:
    MainHostWindow& owner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginListWindow)
};

MainHostWindow::MainHostWindow()
    : juce::DocumentWindow(
          "atkAudio PluginHost2",
          LookAndFeel::getDefaultLookAndFeel().findColour(ResizableWindow::backgroundColourId),
          DocumentWindow::allButtons
      )
{
    // Initialize AudioServer and MidiServer
    audioServer = atk::AudioServer::getInstance();
    midiServer = atk::MidiServer::getInstance();

    if (audioServer)
        audioServer->initialize();
    if (midiServer)
        midiServer->initialize();

    // Don't create MIDI client here - it will be provided externally by ModuleDeviceManager
    // via setExternalMidiClient(), or created on-demand in getMidiClient() if needed

    initialise("");

    // AudioDeviceManager setup is now handled by PluginHost2::Impl via ModuleDeviceManager

    juce::addDefaultFormatsToManager(formatManager);
    formatManager.addFormat(std::make_unique<InternalPluginFormat>());

#if JUCE_LINUX
    // Add Flatpak extension plugin path if it exists
    {
        const File flatpakPluginPath("/app/extensions/Plugins");
        if (flatpakPluginPath.isDirectory())
        {
            if (auto* props = atk::SharedPluginList::getInstance()->getPropertiesFile())
            {
                for (auto* format : formatManager.getFormats())
                {
                    const String formatName = format->getName();
                    // JUCE uses "lastPluginScanPath_" prefix for PluginListComponent
                    const String key = "lastPluginScanPath_" + formatName;
                    FileSearchPath existingPaths(
                        props->getValue(key, format->getDefaultLocationsToSearch().toString())
                    );

                    if (!existingPaths.toString().contains(flatpakPluginPath.getFullPathName()))
                    {
                        existingPaths.add(flatpakPluginPath);
                        props->setValue(key, existingPaths.toString());
                    }
                }
            }
        }
    }
#endif

    setResizable(true, false);
    setResizeLimits(500, 400, 10000, 10000);
    centreWithSize(800, 600);

    // Load plugin list from shared file before creating graph
    atk::SharedPluginList::getInstance()->loadPluginList(knownPluginList);

    graphHolder.reset(new GraphDocumentComponent(*this, formatManager, deviceManager, knownPluginList));

    setContentNonOwned(graphHolder.get(), false);

    // Position title bar buttons on the right (Windows-style), like Plugin Host
    setTitleBarButtonsRequired(DocumentWindow::allButtons, false);

    restoreWindowStateFromString(getAppProperties().getUserSettings()->getValue("mainWindowPos"));

    // Window starts hidden - AudioModule::setVisible() will show it and call toFront()

    InternalPluginFormat internalFormat;
    internalTypes = internalFormat.getAllTypes();

    for (auto& t : internalTypes)
        knownPluginList.addType(t);

    pluginSortMethod = (KnownPluginList::SortMethod)getAppProperties().getUserSettings()->getIntValue(
        "pluginSortMethod",
        KnownPluginList::sortByManufacturer
    );

    knownPluginList.addChangeListener(this);

    if (auto* g = graphHolder->graph.get())
        g->addChangeListener(this);

    addKeyListener(getCommandManager().getKeyMappings());

    Process::setPriority(Process::HighPriority);

    setMenuBar(this);

    getCommandManager().setFirstCommandTarget(this);

    // Window starts off-desktop - AudioModule::setVisible() will add to desktop and show
    removeFromDesktop();
}

MainHostWindow::~MainHostWindow()
{
    // Hide window before destroying components to prevent paint messages during destruction
    setVisible(false);
    removeFromDesktop();

    if (auto* dw = dynamic_cast<juce::DialogWindow*>(audioSettingsDialogWindow))
    {
        if (dw->isCurrentlyModal())
        {
            dw->exitModalState(0);
            delete audioSettingsDialogWindow;
        }
    }

    pluginListWindow = nullptr;
    knownPluginList.removeChangeListener(this);

    if (auto* g = graphHolder->graph.get())
        g->removeChangeListener(this);

    getAppProperties().getUserSettings()->setValue("mainWindowPos", getWindowStateAsString());
    clearContentComponent();

    setMenuBar(nullptr);
    graphHolder = nullptr;
}

void MainHostWindow::closeButtonPressed()
{
    setVisible(false);
}

struct AsyncQuitRetrier final : private Timer
{
    AsyncQuitRetrier()
    {
        startTimer(500);
    }

    void timerCallback() override
    {
        stopTimer();
        delete this;
    }
};

void MainHostWindow::tryToQuitApplication()
{
    // In plugin context, we don't quit the host application (OBS).
    // Instead, attempt to close plugin windows and hide our window.
    if (graphHolder != nullptr)
        (void)graphHolder->closeAnyOpenPluginWindows();

    if (ModalComponentManager::getInstance() != nullptr)
        (void)ModalComponentManager::getInstance()->cancelAllModalComponents();

    setVisible(false);
}

void MainHostWindow::changeListenerCallback(ChangeBroadcaster* changed)
{
    if (changed == &knownPluginList)
    {
        menuItemsChanged();
        atk::SharedPluginList::getInstance()->savePluginList(knownPluginList);
    }
    else if (graphHolder != nullptr && changed == graphHolder->graph.get())
    {
        auto title = juce::String("atkAudio PluginHost2");
        auto f = graphHolder->graph->getFile();

        if (f.existsAsFile())
            title = f.getFileName() + " - " + title;

        setName(title);
    }
}

StringArray MainHostWindow::getMenuBarNames()
{
    StringArray names;
    names.add("File");
    names.add("Plugins");
    names.add("Options");
    names.add("Windows");
    return names;
}

PopupMenu MainHostWindow::getMenuForIndex(int topLevelMenuIndex, const String& /*menuName*/)
{
    PopupMenu menu;

    if (topLevelMenuIndex == 0)
    {
        menu.addCommandItem(&getCommandManager(), CommandIDs::newFile);
        menu.addCommandItem(&getCommandManager(), CommandIDs::open);

        RecentlyOpenedFilesList recentFiles;
        recentFiles.restoreFromString(getAppProperties().getUserSettings()->getValue("recentFilterGraphFiles"));

        PopupMenu recentFilesMenu;
        recentFiles.createPopupMenuItems(recentFilesMenu, 100, true, true);
        menu.addSubMenu("Open recent file", recentFilesMenu);

        // menu.addCommandItem(&getCommandManager(), CommandIDs::save);
        menu.addCommandItem(&getCommandManager(), CommandIDs::saveAs);

        menu.addSeparator();
        menu.addCommandItem(&getCommandManager(), StandardApplicationCommandIDs::quit);
    }
    else if (topLevelMenuIndex == 1)
    {
        // "Plugins" menu
        PopupMenu pluginsMenu;
        addPluginsToMenu(pluginsMenu);
        menu.addSubMenu("Create Plug-in", pluginsMenu);
        menu.addSeparator();
        menu.addItem(250, "Delete All Plug-ins");
    }
    else if (topLevelMenuIndex == 2)
    {
        // "Options" menu

        menu.addCommandItem(&getCommandManager(), CommandIDs::showPluginListEditor);

        PopupMenu sortTypeMenu;
        sortTypeMenu
            .addItem(200, "List Plug-ins in Default Order", true, pluginSortMethod == KnownPluginList::defaultOrder);
        sortTypeMenu.addItem(
            201,
            "List Plug-ins in Alphabetical Order",
            true,
            pluginSortMethod == KnownPluginList::sortAlphabetically
        );
        sortTypeMenu
            .addItem(202, "List Plug-ins by Category", true, pluginSortMethod == KnownPluginList::sortByCategory);
        sortTypeMenu.addItem(
            203,
            "List Plug-ins by Manufacturer",
            true,
            pluginSortMethod == KnownPluginList::sortByManufacturer
        );
        sortTypeMenu.addItem(
            204,
            "List Plug-ins Based on the Directory Structure",
            true,
            pluginSortMethod == KnownPluginList::sortByFileSystemLocation
        );
        menu.addSubMenu("Plug-in Menu Type", sortTypeMenu);

        menu.addSeparator();
        menu.addCommandItem(&getCommandManager(), CommandIDs::showAudioSettings);
        menu.addCommandItem(&getCommandManager(), CommandIDs::showMidiSettings);

        if (autoScaleOptionAvailable)
            menu.addCommandItem(&getCommandManager(), CommandIDs::autoScalePluginWindows);

        menu.addSeparator();
        menu.addCommandItem(&getCommandManager(), CommandIDs::aboutBox);
    }
    else if (topLevelMenuIndex == 3)
    {
        menu.addCommandItem(&getCommandManager(), CommandIDs::allWindowsForward);
    }

    return menu;
}

void MainHostWindow::menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/)
{
    if (menuItemID == 250)
    {
        if (graphHolder != nullptr)
            if (auto* graph = graphHolder->graph.get())
                graph->clear();
    }
    else if (menuItemID >= 100 && menuItemID < 200)
    {
        RecentlyOpenedFilesList recentFiles;
        recentFiles.restoreFromString(getAppProperties().getUserSettings()->getValue("recentFilterGraphFiles"));

        if (graphHolder != nullptr)
        {
            if (auto* graph = graphHolder->graph.get())
            {
                SafePointer<MainHostWindow> parent{this};
                graph->saveIfNeededAndUserAgreesAsync(
                    [parent, recentFiles, menuItemID](FileBasedDocument::SaveResult r)
                    {
                        if (parent == nullptr)
                            return;

                        if (r == FileBasedDocument::savedOk)
                            parent->graphHolder->graph->loadFrom(recentFiles.getFile(menuItemID - 100), true);
                    }
                );
            }
        }
    }
    else if (menuItemID >= 200 && menuItemID < 210)
    {
        if (menuItemID == 200)
            pluginSortMethod = KnownPluginList::defaultOrder;
        else if (menuItemID == 201)
            pluginSortMethod = KnownPluginList::sortAlphabetically;
        else if (menuItemID == 202)
            pluginSortMethod = KnownPluginList::sortByCategory;
        else if (menuItemID == 203)
            pluginSortMethod = KnownPluginList::sortByManufacturer;
        else if (menuItemID == 204)
            pluginSortMethod = KnownPluginList::sortByFileSystemLocation;

        getAppProperties().getUserSettings()->setValue("pluginSortMethod", (int)pluginSortMethod);

        menuItemsChanged();
    }
    else
    {
        if (const auto chosen = getChosenType(menuItemID))
            createPlugin(
                *chosen,
                {proportionOfWidth(0.3f + Random::getSystemRandom().nextFloat() * 0.6f),
                 proportionOfHeight(0.3f + Random::getSystemRandom().nextFloat() * 0.6f)}
            );
    }
}

void MainHostWindow::menuBarActivated(bool isActivated)
{
    if (isActivated && graphHolder != nullptr)
        Component::unfocusAllComponents();
}

void MainHostWindow::createPlugin(const PluginDescriptionAndPreference& desc, Point<int> pos)
{
    if (graphHolder != nullptr)
        graphHolder->createNewPlugin(desc, pos);
}

static bool containsDuplicateNames(const Array<PluginDescription>& plugins, const String& name)
{
    int matches = 0;

    for (auto& p : plugins)
        if (p.name == name && ++matches > 1)
            return true;

    return false;
}

static constexpr int menuIDBase = 0x324503f4;

static void addToMenu(
    const KnownPluginList::PluginTree& tree,
    PopupMenu& m,
    const Array<PluginDescription>& allPlugins,
    Array<PluginDescriptionAndPreference>& addedPlugins
)
{
    for (auto* sub : tree.subFolders)
    {
        PopupMenu subMenu;
        addToMenu(*sub, subMenu, allPlugins, addedPlugins);

        m.addSubMenu(sub->folder, subMenu, true, nullptr, false, 0);
    }

    auto addPlugin = [&](const auto& descriptionAndPreference, const auto& pluginName)
    {
        addedPlugins.add(descriptionAndPreference);
        const auto menuID = addedPlugins.size() - 1 + menuIDBase;
        m.addItem(menuID, pluginName, true, false);
    };

    for (auto& plugin : tree.plugins)
    {
        auto name = plugin.name;

        if (containsDuplicateNames(tree.plugins, name))
            name << " (" << plugin.pluginFormatName << ')';

        addPlugin(PluginDescriptionAndPreference{plugin, PluginDescriptionAndPreference::UseARA::no}, name);

#if JUCE_PLUGINHOST_ARA && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)
        if (plugin.hasARAExtension)
        {
            name << " (ARA)";
            addPlugin(PluginDescriptionAndPreference{plugin}, name);
        }
#endif
    }
}

void MainHostWindow::addPluginsToMenu(PopupMenu& m)
{
    if (graphHolder != nullptr)
    {
        int i = 0;

        for (auto& t : internalTypes)
        {
            // m.addItem(++i, t.name + " (" + t.pluginFormatName + ")");
            m.addItem(++i, t.name);
        }
    }

    m.addSeparator();

    auto pluginDescriptions = knownPluginList.getTypes();

    // This avoids showing the internal types again later on in the list
    pluginDescriptions.removeIf([](PluginDescription& desc)
                                { return desc.pluginFormatName == InternalPluginFormat::getIdentifier(); });

    auto tree = KnownPluginList::createTree(pluginDescriptions, pluginSortMethod);
    pluginDescriptionsAndPreference = {};
    addToMenu(*tree, m, pluginDescriptions, pluginDescriptionsAndPreference);
}

std::optional<PluginDescriptionAndPreference> MainHostWindow::getChosenType(const int menuID) const
{
    const auto internalIndex = menuID - 1;

    if (isPositiveAndBelow(internalIndex, internalTypes.size()))
        return PluginDescriptionAndPreference{internalTypes[(size_t)internalIndex]};

    const auto externalIndex = menuID - menuIDBase;

    if (isPositiveAndBelow(externalIndex, pluginDescriptionsAndPreference.size()))
        return pluginDescriptionsAndPreference[externalIndex];

    return {};
}

ApplicationCommandTarget* MainHostWindow::getNextCommandTarget()
{
    return findFirstTargetParentComponent();
}

void MainHostWindow::getAllCommands(Array<CommandID>& commands)
{
    // this returns the set of all commands that this target can perform..
    const CommandID ids[] = {
        CommandIDs::newFile,
        CommandIDs::open,
        // CommandIDs::save,
        CommandIDs::saveAs,
        CommandIDs::showPluginListEditor,
        CommandIDs::showAudioSettings,
        CommandIDs::showMidiSettings,
        CommandIDs::aboutBox,
        CommandIDs::allWindowsForward,
        CommandIDs::autoScalePluginWindows
    };

    commands.addArray(ids, numElementsInArray(ids));
}

void MainHostWindow::getCommandInfo(const CommandID commandID, ApplicationCommandInfo& result)
{
    const String category("General");

    switch (commandID)
    {
    case CommandIDs::newFile:
        result.setInfo("New", "Creates new filter graph file", category, 0);
        result.defaultKeypresses.add(KeyPress('n', ModifierKeys::commandModifier, 0));
        break;

    case CommandIDs::open:
        result.setInfo("Open...", "Opens filter graph file", category, 0);
        result.defaultKeypresses.add(KeyPress('o', ModifierKeys::commandModifier, 0));
        break;

        // case CommandIDs::save:
        //     result.setInfo("Save", "Saves the current graph to a file", category, 0);
        //     result.defaultKeypresses.add(KeyPress('s', ModifierKeys::commandModifier, 0));
        //     break;

    case CommandIDs::saveAs:
        result.setInfo("Save As...", "Saves copy of current graph to file", category, 0);
        result.defaultKeypresses.add(KeyPress('s', ModifierKeys::shiftModifier | ModifierKeys::commandModifier, 0));
        break;

    case CommandIDs::showPluginListEditor:
        result.setInfo("Edit/Scan List of Available Plug-ins...", {}, category, 0);
        result.addDefaultKeypress('p', ModifierKeys::commandModifier);
        break;

    case CommandIDs::showAudioSettings:
        result.setInfo("Audio...", {}, category, 0);
        result.addDefaultKeypress('a', ModifierKeys::commandModifier);
        break;

    case CommandIDs::showMidiSettings:
        result.setInfo("MIDI...", {}, category, 0);
        result.addDefaultKeypress('m', ModifierKeys::commandModifier);
        break;

    case CommandIDs::aboutBox:
        result.setInfo("About...", {}, category, 0);
        break;

    case CommandIDs::allWindowsForward:
        result.setInfo("All Windows Forward", "Bring all plug-in windows forward", category, 0);
        result.addDefaultKeypress('w', ModifierKeys::commandModifier);
        break;

    case CommandIDs::autoScalePluginWindows:
        updateAutoScaleMenuItem(result);
        break;

    default:
        break;
    }
}

bool MainHostWindow::perform(const InvocationInfo& info)
{
    switch (info.commandID)
    {
    case CommandIDs::newFile:
        if (graphHolder != nullptr && graphHolder->graph != nullptr)
        {
            SafePointer<MainHostWindow> parent{this};
            graphHolder->graph->saveIfNeededAndUserAgreesAsync(
                [parent](FileBasedDocument::SaveResult r)
                {
                    if (parent == nullptr)
                        return;

                    if (r == FileBasedDocument::savedOk)
                        parent->graphHolder->graph->newDocument();
                }
            );
        }
        break;

    case CommandIDs::open:
        if (graphHolder != nullptr && graphHolder->graph != nullptr)
        {
            SafePointer<MainHostWindow> parent{this};
            graphHolder->graph->saveIfNeededAndUserAgreesAsync(
                [parent](FileBasedDocument::SaveResult r)
                {
                    if (parent == nullptr)
                        return;

                    if (r == FileBasedDocument::savedOk)
                        parent->graphHolder->graph->loadFromUserSpecifiedFileAsync(true, [](Result) {});
                }
            );
        }
        break;

        // case CommandIDs::save:
        //     if (graphHolder != nullptr && graphHolder->graph != nullptr)
        //         graphHolder->graph->saveAsync(true, true, nullptr);
        //     break;

    case CommandIDs::saveAs:
        if (graphHolder != nullptr && graphHolder->graph != nullptr)
            graphHolder->graph->saveAsAsync({}, true, true, true, nullptr);
        break;

    case CommandIDs::showPluginListEditor:
        if (pluginListWindow == nullptr)
            pluginListWindow.reset(new PluginListWindow(*this, formatManager));

        pluginListWindow->toFront(true);
        break;

    case CommandIDs::showAudioSettings:
        showAudioSettings();
        break;

    case CommandIDs::showMidiSettings:
        showMidiSettings();
        break;

    case CommandIDs::autoScalePluginWindows:
        if (auto* props = getAppProperties().getUserSettings())
        {
            auto newAutoScale = !isAutoScalePluginWindowsEnabled();
            props->setValue("autoScalePluginWindows", var(newAutoScale));

            ApplicationCommandInfo cmdInfo(info.commandID);
            updateAutoScaleMenuItem(cmdInfo);
            menuItemsChanged();
        }
        break;

    case CommandIDs::aboutBox:
    {
        showAboutDialog();
        break;
    }

    case CommandIDs::allWindowsForward:
    {
        auto& desktop = Desktop::getInstance();

        for (int i = 0; i < desktop.getNumComponents(); ++i)
            desktop.getComponent(i)->toBehind(this);

        break;
    }

    default:
        return false;
    }

    return true;
}

void MainHostWindow::showAudioSettings()
{
    // Use standard JUCE AudioDeviceSelectorComponent
    // ModuleDeviceManager devices will show up with OBS Audio and AudioServer devices
    auto* audioSettingsComp = new AudioDeviceSelectorComponent(
        deviceManager,
        0,     // minAudioInputChannels
        256,   // maxAudioInputChannels
        0,     // minAudioOutputChannels
        256,   // maxAudioOutputChannels
        false, // showMidiInputOptions
        false, // showMidiOutputSelector
        false, // showChannelsAsStereoPairs
        false  // hideAdvancedOptionsWithButton
    );

    audioSettingsComp->setSize(500, 450);

    DialogWindow::LaunchOptions o;
    o.content.setOwned(audioSettingsComp);
    o.dialogTitle = "Audio Settings";
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = getLookAndFeel().findColour(ResizableWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = false;

    auto* w = o.create();
    audioSettingsDialogWindow = w;
    auto safeThis = SafePointer<MainHostWindow>(this);

    w->enterModalState(
        true,
        ModalCallbackFunction::create(
            [safeThis](int)
            {
                // Device changes are handled automatically by AudioDeviceManager

                if (safeThis != nullptr && safeThis->graphHolder != nullptr)
                    if (safeThis->graphHolder->graph != nullptr)
                        safeThis->graphHolder->graph->graph.removeIllegalConnections();
            }
        ),
        true
    );
}

void MainHostWindow::showMidiSettings()
{
    // Create MIDI settings component using ModuleDeviceManager's MidiClient
    auto* midiSettingsComp = new atk::MidiServerSettingsComponent(&getMidiClient());

    midiSettingsComp->setSize(600, 550);

    DialogWindow::LaunchOptions o;
    o.content.setOwned(midiSettingsComp);
    o.dialogTitle = "MIDI Settings";
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = getLookAndFeel().findColour(ResizableWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = false;

    auto* w = o.create();
    midiSettingsDialogWindow = w;
    auto safeThis = SafePointer<MainHostWindow>(this);

    w->enterModalState(
        true,
        ModalCallbackFunction::create(
            [safeThis](int)
            {
                // MIDI device subscriptions are managed by MidiServer
                // No additional cleanup needed here
            }
        ),
        true
    );
}

bool MainHostWindow::isInterestedInFileDrag(const StringArray&)
{
    return true;
}

void MainHostWindow::fileDragEnter(const StringArray&, int, int)
{
}

void MainHostWindow::fileDragMove(const StringArray&, int, int)
{
}

void MainHostWindow::fileDragExit(const StringArray&)
{
}

void MainHostWindow::filesDropped(const StringArray& files, int x, int y)
{
    if (graphHolder != nullptr)
    {
        File firstFile{files[0]};

        if (files.size() == 1 && firstFile.hasFileExtension(PluginGraph::getFilenameSuffix()))
        {
            if (auto* g = graphHolder->graph.get())
            {
                SafePointer<MainHostWindow> parent;
                g->saveIfNeededAndUserAgreesAsync(
                    [parent, g, firstFile](FileBasedDocument::SaveResult r)
                    {
                        if (parent == nullptr)
                            return;

                        if (r == FileBasedDocument::savedOk)
                            g->loadFrom(firstFile, true);
                    }
                );
            }
        }
        else
        {
            OwnedArray<PluginDescription> typesFound;
            knownPluginList.scanAndAddDragAndDroppedFiles(formatManager, files, typesFound);

            auto pos = graphHolder->getLocalPoint(this, Point<int>(x, y));

            for (int i = 0; i < jmin(5, typesFound.size()); ++i)
                if (auto* desc = typesFound.getUnchecked(i))
                    createPlugin(PluginDescriptionAndPreference{*desc}, pos);
        }
    }
}

bool MainHostWindow::isAutoScalePluginWindowsEnabled()
{
    if (auto* props = getAppProperties().getUserSettings())
        return props->getBoolValue("autoScalePluginWindows", false);

    return false;
}

void MainHostWindow::updateAutoScaleMenuItem(ApplicationCommandInfo& info)
{
    info.setInfo("Auto-Scale Plug-in Windows", {}, "General", 0);
    info.setTicked(isAutoScalePluginWindowsEnabled());
}
