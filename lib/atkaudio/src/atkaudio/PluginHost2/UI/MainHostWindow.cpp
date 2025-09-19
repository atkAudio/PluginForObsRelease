#include "MainHostWindow.h"

#include "../../About.h"
#include "../Plugins/InternalPlugins.h"
#include "../VirtualAudioIoDevice.h"
#include "../ph2_AudioDeviceSelectorComponent.h"

constexpr const char* scanModeKey = "pluginScanMode";

//==============================================================================
class Superprocess final : private ChildProcessCoordinator
{
public:
    Superprocess()
    {
        launchWorkerProcess(File::getSpecialLocation(File::currentExecutableFile), processUID, 0, 0);
    }

    enum class State
    {
        timeout,
        gotResult,
        connectionLost,
    };

    struct Response
    {
        State state;
        std::unique_ptr<XmlElement> xml;
    };

    Response getResponse()
    {
        std::unique_lock<std::mutex> lock{mutex};

        if (!condvar.wait_for(lock, std::chrono::milliseconds{50}, [&] { return gotResult || connectionLost; }))
            return {State::timeout, nullptr};

        const auto state = connectionLost ? State::connectionLost : State::gotResult;
        connectionLost = false;
        gotResult = false;

        return {state, std::move(pluginDescription)};
    }

    using ChildProcessCoordinator::sendMessageToWorker;

private:
    void handleMessageFromWorker(const MemoryBlock& mb) override
    {
        const std::lock_guard<std::mutex> lock{mutex};
        pluginDescription = parseXML(mb.toString());
        gotResult = true;
        condvar.notify_one();
    }

    void handleConnectionLost() override
    {
        const std::lock_guard<std::mutex> lock{mutex};
        connectionLost = true;
        condvar.notify_one();
    }

    std::mutex mutex;
    std::condition_variable condvar;

    std::unique_ptr<XmlElement> pluginDescription;
    bool connectionLost = false;
    bool gotResult = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Superprocess)
};

//==============================================================================
class CustomPluginScanner final
    : public KnownPluginList::CustomScanner
    , private ChangeListener
{
public:
    CustomPluginScanner(MainHostWindow& mainHostWindow)
        : mw(mainHostWindow)
    {
        if (auto* file = mw.getAppProperties().getUserSettings())
            file->addChangeListener(this);

        handleChange();
    }

    ~CustomPluginScanner() override
    {
        if (auto* file = mw.getAppProperties().getUserSettings())
            file->removeChangeListener(this);
    }

    bool findPluginTypesFor(
        AudioPluginFormat& format,
        OwnedArray<PluginDescription>& result,
        const String& fileOrIdentifier
    ) override
    {
        if (scanInProcess)
        {
            superprocess = nullptr;
            format.findAllTypesForFile(result, fileOrIdentifier);
            return true;
        }

        if (addPluginDescriptions(format.getName(), fileOrIdentifier, result))
            return true;

        superprocess = nullptr;
        return false;
    }

    void scanFinished() override
    {
        superprocess = nullptr;
    }

private:
    MainHostWindow& mw;

    /*  Scans for a plugin with format 'formatName' and ID 'fileOrIdentifier' using a subprocess,
        and adds discovered plugin descriptions to 'result'.

        Returns true on success.

        Failure indicates that the subprocess is unrecoverable and should be terminated.
    */
    bool addPluginDescriptions(
        const String& formatName,
        const String& fileOrIdentifier,
        OwnedArray<PluginDescription>& result
    )
    {
        if (superprocess == nullptr)
            superprocess = std::make_unique<Superprocess>();

        MemoryBlock block;
        MemoryOutputStream stream{block, true};
        stream.writeString(formatName);
        stream.writeString(fileOrIdentifier);

        if (!superprocess->sendMessageToWorker(block))
            return false;

        for (;;)
        {
            if (shouldExit())
                return true;

            const auto response = superprocess->getResponse();

            if (response.state == Superprocess::State::timeout)
                continue;

            if (response.xml != nullptr)
            {
                for (const auto* item : response.xml->getChildIterator())
                {
                    auto desc = std::make_unique<PluginDescription>();

                    if (desc->loadFromXml(*item))
                        result.add(std::move(desc));
                }
            }

            return (response.state == Superprocess::State::gotResult);
        }
    }

    void handleChange()
    {
        if (auto* file = mw.getAppProperties().getUserSettings())
            scanInProcess = (file->getIntValue(scanModeKey) == 0);
    }

    void changeListenerCallback(ChangeBroadcaster*) override
    {
        handleChange();
    }

    std::unique_ptr<Superprocess> superprocess;

    std::atomic<bool> scanInProcess{true};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomPluginScanner)
};

//==============================================================================
class CustomPluginListComponent final : public PluginListComponent
{
public:
    CustomPluginListComponent(
        MainHostWindow& mainWindow,
        AudioPluginFormatManager& manager,
        KnownPluginList& listToRepresent,
        const File& pedal,
        PropertiesFile* props,
        bool async
    )
        : PluginListComponent(manager, listToRepresent, pedal, props, async)
        , mw(mainWindow)
    {
        addAndMakeVisible(validationModeLabel);
        addAndMakeVisible(validationModeBox);

        validationModeLabel.attachToComponent(&validationModeBox, true);
        validationModeLabel.setJustificationType(Justification::right);
        validationModeLabel.setSize(100, 30);

        auto unusedId = 1;

        for (const auto mode : {"In-process", "Out-of-process"})
            validationModeBox.addItem(mode, unusedId++);

        validationModeBox.setSelectedItemIndex(mw.getAppProperties().getUserSettings()->getIntValue(scanModeKey));

        validationModeBox.onChange = [this]
        { mw.getAppProperties().getUserSettings()->setValue(scanModeKey, validationModeBox.getSelectedItemIndex()); };

        handleResize();
    }

    void resized() override
    {
        handleResize();
    }

private:
    void handleResize()
    {
        PluginListComponent::resized();

        const auto& buttonBounds = getOptionsButton().getBounds();
        validationModeBox.setBounds(buttonBounds.withWidth(130).withRightX(getWidth() - buttonBounds.getX()));
    }

    MainHostWindow& mw;

    Label validationModeLabel{{}, "Scan mode"};
    ComboBox validationModeBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomPluginListComponent)
};

//==============================================================================
class MainHostWindow::PluginListWindow final : public DocumentWindow
{
public:
    PluginListWindow(MainHostWindow& mw, AudioPluginFormatManager& pluginFormatManager)
        : DocumentWindow(
              "Available Plugins",
              LookAndFeel::getDefaultLookAndFeel().findColour(ResizableWindow::backgroundColourId),
              DocumentWindow::minimiseButton | DocumentWindow::closeButton
          )
        , owner(mw)
    {
        auto deadMansPedalFile =
            owner.getAppProperties().getUserSettings()->getFile().getSiblingFile("RecentlyCrashedPluginsList");

        setContentOwned(
            new CustomPluginListComponent(
                owner,
                pluginFormatManager,
                owner.knownPluginList,
                deadMansPedalFile,
                owner.getAppProperties().getUserSettings(),
                false // sync scan
            ),
            true
        );

        setResizable(true, false);
        setResizeLimits(300, 400, 800, 1500);
        setTopLeftPosition(60, 60);

        // restoreWindowStateFromString(owner.getAppProperties().getUserSettings()->getValue("listWindowPos"));
        setVisible(true);
    }

    ~PluginListWindow() override
    {
        // owner.getAppProperties().getUserSettings()->setValue("listWindowPos", getWindowStateAsString());
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

//==============================================================================
MainHostWindow::MainHostWindow()
    : DocumentWindow(
          "atkAudio Plugin Host2",
          LookAndFeel::getDefaultLookAndFeel().findColour(ResizableWindow::backgroundColourId),
          DocumentWindow::allButtons,
          false
      )
{
    initialise("");

    auto& dm = getDeviceManager();
    dm.initialiseWithDefaultDevices(256, 256);
    dm.addAudioDeviceType(std::make_unique<VirtualAudioIODeviceType>());
    dm.setCurrentAudioDeviceType(IO_TYPE, true);
    juce::AudioDeviceManager::AudioDeviceSetup setup = dm.getAudioDeviceSetup();
    setup.inputDeviceName = IO_NAME;
    setup.outputDeviceName = IO_NAME;
    dm.setAudioDeviceSetup(setup, true);

    formatManager.addDefaultFormats();
    formatManager.addFormat(new InternalPluginFormat());

    setResizable(true, false);
    setResizeLimits(500, 400, 10000, 10000);
    centreWithSize(800, 600);

    knownPluginList.setCustomScanner(std::make_unique<CustomPluginScanner>(*this));

    graphHolder.reset(new GraphDocumentComponent(*this, formatManager, deviceManager, knownPluginList));

    setContentNonOwned(graphHolder.get(), false);

    // Position title bar buttons on the right (Windows-style), like Plugin Host
    setTitleBarButtonsRequired(DocumentWindow::allButtons, false);

    restoreWindowStateFromString(getAppProperties().getUserSettings()->getValue("mainWindowPos"));

    setVisible(false);

    InternalPluginFormat internalFormat;
    internalTypes = internalFormat.getAllTypes();

    if (auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue("pluginList"))
        knownPluginList.recreateFromXml(*savedPluginList);

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
}

MainHostWindow::~MainHostWindow()
{
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
    if (graphHolder->closeAnyOpenPluginWindows())
    {
        // Really important thing to note here: if the last call just deleted any plugin windows,
        // we won't exit immediately - instead we'll use our AsyncQuitRetrier to let the message
        // loop run for another brief moment, then try again. This will give any plugins a chance
        // to flush any GUI events that may have been in transit before the app forces them to
        // be unloaded
        new AsyncQuitRetrier();
        return;
    }

    if (ModalComponentManager::getInstance()->cancelAllModalComponents())
    {
        new AsyncQuitRetrier();
        return;
    }

    if (graphHolder != nullptr)
    {
        auto releaseAndQuit = [this]
        {
            // Some plug-ins do not want [NSApp stop] to be called
            // before the plug-ins are not deallocated.
            graphHolder->releaseGraph();
        };

        SafePointer<MainHostWindow> parent{this};
        graphHolder->graph->saveIfNeededAndUserAgreesAsync(
            [parent, releaseAndQuit](FileBasedDocument::SaveResult r)
            {
                if (parent == nullptr)
                    return;

                if (r == FileBasedDocument::savedOk)
                    releaseAndQuit();
            }
        );

        return;
    }

    JUCEApplication::quit();
}

void MainHostWindow::changeListenerCallback(ChangeBroadcaster* changed)
{
    if (changed == &knownPluginList)
    {
        menuItemsChanged();

        // save the plugin list every time it gets changed, so that if we're scanning
        // and it crashes, we've still saved the previous ones
        if (auto savedPluginList = std::unique_ptr<XmlElement>(knownPluginList.createXml()))
        {
            getAppProperties().getUserSettings()->setValue("pluginList", savedPluginList.get());
            getAppProperties().saveIfNeeded();
        }
    }
    else if (graphHolder != nullptr && changed == graphHolder->graph.get())
    {
        auto title = juce::String("atkAudio Plugin Host2");
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
        menu.addCommandItem(&getCommandManager(), CommandIDs::toggleDoublePrecision);

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

//==============================================================================
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
        CommandIDs::toggleDoublePrecision,
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
        result.setInfo("Change Device Settings", {}, category, 0);
        result.addDefaultKeypress('a', ModifierKeys::commandModifier);
        break;

    case CommandIDs::toggleDoublePrecision:
        updatePrecisionMenuItem(result);
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

    case CommandIDs::toggleDoublePrecision:
        if (auto* props = getAppProperties().getUserSettings())
        {
            auto newIsDoublePrecision = !isDoublePrecisionProcessingEnabled();
            props->setValue("doublePrecisionProcessing", var(newIsDoublePrecision));

            ApplicationCommandInfo cmdInfo(info.commandID);
            updatePrecisionMenuItem(cmdInfo);
            menuItemsChanged();

            if (graphHolder != nullptr)
                graphHolder->setDoublePrecision(newIsDoublePrecision);
        }
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
    auto* audioSettingsComp =
        new ph2::AudioDeviceSelectorComponent(deviceManager, 0, 256, 0, 256, true, true, true, true);

    audioSettingsComp->setSize(500, 450);

    DialogWindow::LaunchOptions o;
    o.content.setOwned(audioSettingsComp);

    o.content.setOwned(audioSettingsComp);
    o.dialogTitle = "Device Settings";
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = getLookAndFeel().findColour(ResizableWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = false;

    // audioSettingsDialogWindow.reset(o.launchAsync());
    auto* w = o.create();
    audioSettingsDialogWindow = w;
    auto safeThis = SafePointer<MainHostWindow>(this);

    w->enterModalState(
        true,
        ModalCallbackFunction::create(
            [safeThis](int)
            {
                // auto audioState = safeThis->deviceManager.createStateXml();

                // safeThis->getAppProperties().getUserSettings()->setValue("audioDeviceState", audioState.get());
                // safeThis->getAppProperties().getUserSettings()->saveIfNeeded();

                // if (safeThis->graphHolder != nullptr)
                //     if (safeThis->graphHolder->graph != nullptr)
                //         safeThis->graphHolder->graph->graph.removeIllegalConnections();
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

bool MainHostWindow::isDoublePrecisionProcessingEnabled()
{
    if (auto* props = getAppProperties().getUserSettings())
        return props->getBoolValue("doublePrecisionProcessing", false);

    return false;
}

bool MainHostWindow::isAutoScalePluginWindowsEnabled()
{
    if (auto* props = getAppProperties().getUserSettings())
        return props->getBoolValue("autoScalePluginWindows", false);

    return false;
}

void MainHostWindow::updatePrecisionMenuItem(ApplicationCommandInfo& info)
{
    info.setInfo("Double Floating-Point Precision Rendering", {}, "General", 0);
    info.setTicked(isDoublePrecisionProcessingEnabled());
}

void MainHostWindow::updateAutoScaleMenuItem(ApplicationCommandInfo& info)
{
    info.setInfo("Auto-Scale Plug-in Windows", {}, "General", 0);
    info.setTicked(isAutoScalePluginWindowsEnabled());
}
