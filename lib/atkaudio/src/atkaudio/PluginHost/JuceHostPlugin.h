#pragma once
#include "../LookAndFeel.h"

#include <atkaudio/atkaudio.h>
#include <juce_audio_utils/juce_audio_utils.h>

using namespace juce;

//==============================================================================
enum class EditorStyle
{
    thisWindow,
    newWindow
};

class HostAudioProcessorImpl
    : public AudioProcessor
    , private ChangeListener
{
    static inline juce::InterProcessLock appPropertiesLock{"pluginHostAppPropertiesLock"};

public:
    HostAudioProcessorImpl()
        : AudioProcessor(
              BusesProperties()
                  .withInput("Input", AudioChannelSet::stereo(), true)
                  .withOutput("Output", AudioChannelSet::stereo(), true)
                  .withInput("Sidechain", AudioChannelSet::stereo(), false)
          )

    {
        appProperties.setStorageParameters(
            [&]
            {
                PropertiesFile::Options opt;
                opt.applicationName = getName();
                opt.commonToAllUsers = false;
                opt.doNotSave = false;
                opt.filenameSuffix = "settings";
                opt.ignoreCaseOfKeyNames = false;
                opt.storageFormat = PropertiesFile::StorageFormat::storeAsXML;
                opt.osxLibrarySubFolder = "Application Support";
                opt.processLock = &appPropertiesLock;
                return opt;
            }()
        );

        pluginFormatManager.addDefaultFormats();

        if (auto savedPluginList = appProperties.getUserSettings()->getXmlValue("pluginList"))
            pluginList.recreateFromXml(*savedPluginList);

        MessageManagerLock lock;
        pluginList.addChangeListener(this);
    }

    ~HostAudioProcessorImpl() override
    {
    }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const final
    {
        const auto& mainOutput = layouts.getMainOutputChannelSet();
        const auto& mainInput = layouts.getMainInputChannelSet();

        if (!mainInput.isDisabled() && mainInput != mainOutput)
            return false;

        if (mainOutput.size() > 8)
            return false;

        return true;
    }

    void prepareToPlay(double sr, int bs) final
    {
        const ScopedLock sl(innerMutex);

        active = true;

        if (inner != nullptr)
        {
            inner->setRateAndBufferSizeDetails(sr, bs);
            inner->prepareToPlay(sr, bs);
        }
    }

    void releaseResources() final
    {
        const ScopedLock sl(innerMutex);

        active = false;

        if (inner != nullptr)
            inner->releaseResources();
    }

    void reset() final
    {
        const ScopedLock sl(innerMutex);

        if (inner != nullptr)
            inner->reset();
    }

    class AtkAudioPlayHead : public juce::AudioPlayHead
    {
    public:
        juce::AudioPlayHead::PositionInfo positionInfo;

        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
        {
            return positionInfo;
        }
    };

    AtkAudioPlayHead atkPlayHead;

    void processBlock(AudioBuffer<float>& buffer, MidiBuffer& midiBuffer) final
    {
        jassert(!isUsingDoublePrecision());
        if (inner == nullptr)
            return;

        juce::AudioBuffer<float> tempBuffer;

        int numChannels = 0;
        for (int i = 0; i < inner->getBusCount(true); ++i)
            numChannels += inner->getChannelCountOfBus(true, i);

        if (numChannels == 0)
            return;

        atkPlayHead.positionInfo.setIsPlaying(true);
        atkPlayHead.positionInfo.setBpm(120.0);
        auto pos =
            atkPlayHead.positionInfo.getTimeInSamples().hasValue() ? *atkPlayHead.positionInfo.getTimeInSamples() : 0;
        atkPlayHead.positionInfo.setTimeInSamples(pos + buffer.getNumSamples());
        inner->setPlayHead(&atkPlayHead);

        tempBuffer.setDataToReferTo(buffer.getArrayOfWritePointers(), numChannels, buffer.getNumSamples());
        inner->processBlock(tempBuffer, midiBuffer);
    }

    void processBlock(AudioBuffer<double>&, MidiBuffer&) final
    {
        jassert(isUsingDoublePrecision());
    }

    bool hasEditor() const override
    {
        return false;
    }

    AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }

    const String getName() const final
    {
        return "atkAudio Plugin Host";
    }

    bool acceptsMidi() const final
    {
        return true;
    }

    bool producesMidi() const final
    {
        return true;
    }

    double getTailLengthSeconds() const final
    {
        return 0.0;
    }

    int getNumPrograms() final
    {
        return 0;
    }

    int getCurrentProgram() final
    {
        return 0;
    }

    void setCurrentProgram(int) final
    {
    }

    const String getProgramName(int) final
    {
        return "None";
    }

    void changeProgramName(int, const String&) final
    {
    }

    void getStateInformation(MemoryBlock& destData) final
    {
        const ScopedLock sl(innerMutex);

        XmlElement xml("state");

        if (inner != nullptr)
        {
            xml.setAttribute(editorStyleTag, (int)editorStyle);
            xml.addChildElement(inner->getPluginDescription().createXml().release());
            xml.addChildElement(
                [this]
                {
                    MemoryBlock innerState;
                    inner->getStateInformation(innerState);

                    auto stateNode = std::make_unique<XmlElement>(innerStateTag);
                    stateNode->addTextElement(innerState.toBase64Encoding());
                    return stateNode.release();
                }()
            );
        }

        const auto text = xml.toString();
        destData.replaceAll(text.toRawUTF8(), text.getNumBytesAsUTF8());
    }

    void setStateInformation(const void* data, int sizeInBytes) final
    {
        const ScopedLock sl(innerMutex);

        auto xml = XmlDocument::parse(String(CharPointer_UTF8(static_cast<const char*>(data)), (size_t)sizeInBytes));

        if (auto* pluginNode = xml->getChildByName("PLUGIN"))
        {
            PluginDescription pd;
            pd.loadFromXml(*pluginNode);

            MemoryBlock innerState;
            innerState.fromBase64Encoding(xml->getChildElementAllSubText(innerStateTag, {}));

            setNewPlugin(pd, (EditorStyle)xml->getIntAttribute(editorStyleTag, 0), innerState);
        }
    }

    void setNewPlugin(const PluginDescription& pd, EditorStyle where, const MemoryBlock& mb = {})
    {
        const ScopedLock sl(innerMutex);

        const auto callback = [this, where, mb](std::unique_ptr<AudioPluginInstance> instance, const String& error)
        {
            if (error.isNotEmpty())
            {
                auto options =
                    MessageBoxOptions::makeOptionsOk(MessageBoxIconType::WarningIcon, "Plugin Load Failed", error);
                messageBox = AlertWindow::showScopedAsync(options, nullptr);
                return;
            }

            bool needsPluginChanged = false;
            String innerName;
            if (inner != nullptr)
                innerName = inner->getPluginDescription().descriptiveName;

            if (innerName != instance->getPluginDescription().descriptiveName)
                needsPluginChanged = true;

            if (needsPluginChanged)
                inner = std::move(instance);

            editorStyle = where;

            // In a 'real' plugin, we'd also need to set the bus configuration of the inner plugin.
            // One possibility would be to match the bus configuration of the wrapper plugin, but
            // the inner plugin isn't guaranteed to support the same layout. Alternatively, we
            // could try to apply a reasonably similar layout, and maintain a mapping between the
            // inner/outer channel layouts.
            //
            // In any case, it is essential that the inner plugin is told about the bus
            // configuration that will be used. The AudioBuffer passed to the inner plugin must also
            // exactly match this layout.

            if (active)
            {
                bool layoutSupported = false;
                auto layout = getBusesLayout();
                // check with sidechain bus
                if (inner->checkBusesLayoutSupported(layout))
                {
                    inner->setBusesLayout(layout);
                    inner->setRateAndBufferSizeDetails(getSampleRate(), getBlockSize());
                    layoutSupported = true;
                }

                // try stereo sidechain bus
                if (!layoutSupported)
                {
                    layout.inputBuses.removeLast();
                    layout.inputBuses.add(juce::AudioChannelSet::stereo());
                    if (!layoutSupported && inner->checkBusesLayoutSupported(layout))
                    {
                        inner->setBusesLayout(layout);
                        inner->setRateAndBufferSizeDetails(getSampleRate(), getBlockSize());
                        layoutSupported = true;
                    }
                }

                // try mono sidechain bus
                if (!layoutSupported)
                {
                    layout.inputBuses.removeLast();
                    layout.inputBuses.add(juce::AudioChannelSet::mono());
                    if (!layoutSupported && inner->checkBusesLayoutSupported(layout))
                    {
                        inner->setBusesLayout(layout);
                        inner->setRateAndBufferSizeDetails(getSampleRate(), getBlockSize());
                        layoutSupported = true;
                    }
                }

                // try no sidechain bus
                if (!layoutSupported)
                {
                    layout.inputBuses.removeLast();
                    if (!layoutSupported && inner->checkBusesLayoutSupported(layout))
                    {
                        inner->setBusesLayout(layout);
                        inner->setRateAndBufferSizeDetails(getSampleRate(), getBlockSize());
                        layoutSupported = true;
                    }
                }

                if (!layoutSupported)
                {
                    inner->setPlayConfigDetails(
                        getMainBusNumInputChannels(),
                        getMainBusNumOutputChannels(),
                        getSampleRate(),
                        getBlockSize()
                    );
                }
            }

            this->prepareToPlay(getSampleRate(), getBlockSize());

            if (inner != nullptr && !mb.isEmpty())
                inner->setStateInformation(mb.getData(), (int)mb.getSize());

            if (needsPluginChanged)
                NullCheckedInvocation::invoke(pluginChanged);
        };

        if (inner == nullptr || (inner != nullptr && inner->getPluginDescription().name != pd.name))
            pluginFormatManager.createPluginInstanceAsync(pd, getSampleRate(), getBlockSize(), callback);
    }

    void clearPlugin()
    {
        const ScopedLock sl(innerMutex);

        inner = nullptr;
        NullCheckedInvocation::invoke(pluginChanged);
    }

    bool isPluginLoaded() const
    {
        const ScopedLock sl(innerMutex);
        return inner != nullptr;
    }

    std::unique_ptr<AudioProcessorEditor> createInnerEditor() const
    {
        const ScopedLock sl(innerMutex);
        return rawToUniquePtr(inner->hasEditor() ? inner->createEditorIfNeeded() : nullptr);
    }

    EditorStyle getEditorStyle() const noexcept
    {
        return editorStyle;
    }

    ApplicationProperties appProperties;
    AudioPluginFormatManager pluginFormatManager;
    KnownPluginList pluginList;
    std::function<void()> pluginChanged;

private:
    CriticalSection innerMutex;
    std::unique_ptr<AudioPluginInstance> inner;
    EditorStyle editorStyle = EditorStyle{};
    bool active = false;
    ScopedMessageBox messageBox;

    static constexpr const char* innerStateTag = "inner_state";
    static constexpr const char* editorStyleTag = "editor_style";

    void changeListenerCallback(ChangeBroadcaster* source) final
    {
        if (source != &pluginList)
            return;

        if (auto savedPluginList = pluginList.createXml())
        {
            appProperties.getUserSettings()->setValue("pluginList", savedPluginList.get());
            appProperties.saveIfNeeded();
        }
    }
};

//==============================================================================
constexpr auto margin = 10;

static void doLayout(Component* main, Component& bottom, int bottomHeight, Rectangle<int> bounds)
{
    Grid grid;
    grid.setGap(Grid::Px{margin});
    grid.templateColumns = {Grid::TrackInfo{Grid::Fr{1}}};
    grid.templateRows = {Grid::TrackInfo{Grid::Fr{1}}, Grid::TrackInfo{Grid::Px{bottomHeight}}};

    grid.items = {
        GridItem{main},
        GridItem{bottom}.withMargin({0, margin, margin, margin}),
        //
    };
    grid.performLayout(bounds);
}

class PluginLoaderComponent final : public Component
{
public:
    template <typename Callback>
    PluginLoaderComponent(AudioPluginFormatManager& manager, KnownPluginList& list, Callback&& callback)
        : pluginListComponent(manager, list, {}, {})
    {
        pluginListComponent.getTableListBox().setMultipleSelectionEnabled(false);

        addAndMakeVisible(pluginListComponent);
        addAndMakeVisible(buttons);

        const auto getCallback = [this, &list, cb = std::forward<Callback>(callback)](EditorStyle style)
        {
            return [this, &list, cb, style]
            {
                const auto index = pluginListComponent.getTableListBox().getSelectedRow();
                const auto& types = list.getTypes();

                if (isPositiveAndBelow(index, types.size()))
                    NullCheckedInvocation::invoke(cb, types.getReference(index), style);
            };
        };

        buttons.thisWindowButton.onClick = getCallback(EditorStyle::thisWindow);
        // buttons.newWindowButton.onClick = getCallback(EditorStyle::newWindow);
    }

    void resized() override
    {
        doLayout(&pluginListComponent, buttons, 80, getLocalBounds());
    }

private:
    struct Buttons final : public Component
    {
        Buttons()
        {
            label.setJustificationType(Justification::centred);

            addAndMakeVisible(label);
            addAndMakeVisible(thisWindowButton);
            // addAndMakeVisible(newWindowButton);
        }

        void resized() override
        {
            Grid vertical;
            vertical.autoFlow = Grid::AutoFlow::row;
            vertical.setGap(Grid::Px{margin});
            vertical.autoRows = vertical.autoColumns = Grid::TrackInfo{Grid::Fr{1}};
            vertical.items.insertMultiple(0, GridItem{}, 2);
            vertical.performLayout(getLocalBounds());

            label.setBounds(vertical.items[0].currentBounds.toNearestInt());

            Grid grid;
            grid.autoFlow = Grid::AutoFlow::column;
            grid.setGap(Grid::Px{margin});
            grid.autoRows = grid.autoColumns = Grid::TrackInfo{Grid::Fr{1}};
            grid.items = {GridItem{thisWindowButton}}; //, GridItem{newWindowButton}};

            grid.performLayout(vertical.items[1].currentBounds.toNearestInt());
            thisWindowButton.changeWidthToFitText();
            thisWindowButton.setTopLeftPosition(
                (getWidth() - thisWindowButton.getWidth()) / 2,
                thisWindowButton.getY() - 5
            );
        }

        Label label{"", "Select a plugin from the list, then load it."};
        TextButton thisWindowButton{"Load plugin"};
        // TextButton newWindowButton{"Open In New Window"};
    };

    PluginListComponent pluginListComponent;
    Buttons buttons;
};

//==============================================================================
class PluginEditorComponent final : public Component
{
public:
    template <typename Callback>
    PluginEditorComponent(std::unique_ptr<AudioProcessorEditor> editorIn, Callback&& onClose)
        : editor(std::move(editorIn))
    {
        addAndMakeVisible(editor.get());
        // addAndMakeVisible(closeButton);
        addAndMakeVisible(buttons);

        childBoundsChanged(editor.get());

        auto lambda = [onClose]
        {
            AlertWindow::showOkCancelBox(
                AlertWindow::WarningIcon,
                "Unload Plugin",
                "Are you sure you want to unload the plugin?",
                "Yes",
                "No",
                nullptr,
                ModalCallbackFunction::create(
                    [onClose](int result)
                    {
                        if (result == 1) // 'Yes' was clicked
                            onClose();
                    }
                )
            );
        };
        // closeButton.onClick = lambda;
        buttons.closeButton.onClick = lambda;
        // buttons.closeButton.setSize(
        //     buttons.closeButton.getBestWidthForHeight(buttonHeight),
        //     buttons.closeButton.getHeight()
        // );

        // closeButton.setSize(closeButton.getBestWidthForHeight(buttonHeight), closeButton.getHeight());
    }

    void setScaleFactor(float scale)
    {
        if (editor != nullptr)
            editor->setScaleFactor(scale);
    }

    void resized() override
    {
        doLayout(editor.get(), buttons, buttonHeight, getLocalBounds());
    }

    void childBoundsChanged(Component* child) override
    {
        if (child != editor.get())
            return;

        const auto size = editor != nullptr ? editor->getLocalBounds() : Rectangle<int>();

        setSize(size.getWidth(), margin + buttonHeight + size.getHeight());
    }

private:
    static constexpr auto buttonHeight = 40;

    std::unique_ptr<AudioProcessorEditor> editor;

    struct Buttons2 final : public Component
    {
        Buttons2()
        {
            addAndMakeVisible(closeButton);
            addAndMakeVisible(linkButton);
        }

        void resized() override
        {
            Grid grid;
            grid.autoFlow = Grid::AutoFlow::column;
            grid.setGap(Grid::Px{margin});
            grid.autoRows = grid.autoColumns = Grid::TrackInfo{Grid::Fr{1}};
            grid.items = {
                GridItem{closeButton}
                    .withSize((float)closeButton.getBestWidthForHeight(buttonHeight), (float)getHeight()),
                GridItem{linkButton}
            };

            grid.performLayout(getLocalBounds());
            linkButton.changeWidthToFitText();
            linkButton.setTopRightPosition(getWidth(), 0);
        }

        TextButton closeButton{"Unload Plugin"};
        HyperlinkButton linkButton{"atkAudio", URL("http://www.atkaudio.com")};
    };

    Buttons2 buttons;
};

//==============================================================================
class ScaledDocumentWindow final : public DocumentWindow
{
public:
    ScaledDocumentWindow(Colour bg, float scale)
        : DocumentWindow("Editor", bg, 0)
        , desktopScale(scale)
    {
    }

    float getDesktopScaleFactor() const override
    {
        return Desktop::getInstance().getGlobalScaleFactor() * desktopScale;
    }

private:
    float desktopScale = 1.0f;
};

//==============================================================================
class HostAudioProcessorEditor final : public AudioProcessorEditor
{
public:
    explicit HostAudioProcessorEditor(HostAudioProcessorImpl& owner)
        : AudioProcessorEditor(owner)
        , hostProcessor(owner)
        , loader(
              owner.pluginFormatManager,
              owner.pluginList,
              [&owner](const PluginDescription& pd, EditorStyle editorStyle) { owner.setNewPlugin(pd, editorStyle); }
          )
        , scopedCallback(owner.pluginChanged, [this] { pluginChanged(); })
    {
        currentScaleFactor =
            juce::Desktop::getInstance().getDisplays().getDisplayForRect(getLocalBounds())->dpi / atk::DPI_NORMAL;

        setSize(500, 500);
        setResizable(false, false);
        addAndMakeVisible(closeButton);
        addAndMakeVisible(loader);

        hostProcessor.pluginChanged();

        closeButton.onClick = [this] { clearPlugin(); };
    }

    void parentSizeChanged() override
    {
    }

    void paint(Graphics& g) override
    {
        g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId).darker());
    }

    void resized() override
    {
        closeButton.setBounds(getLocalBounds().withSizeKeepingCentre(200, buttonHeight));
        loader.setBounds(getLocalBounds());
    }

    void childBoundsChanged(Component* child) override
    {
        if (child != editor.get())
            return;

        const auto size = editor != nullptr ? editor->getLocalBounds() : Rectangle<int>();

        setSize(size.getWidth(), size.getHeight());
    }

    void setScaleFactor(float scale) override
    {
        currentScaleFactor = scale;
        AudioProcessorEditor::setScaleFactor(scale);

        [[maybe_unused]] const auto posted = MessageManager::callAsync(
            [ref = SafePointer<HostAudioProcessorEditor>(this), scale]
            {
                if (auto* r = ref.getComponent())
                    if (auto* e = r->currentEditorComponent)
                        e->setScaleFactor(scale);
            }
        );

        jassert(posted);
    }

private:
    void pluginChanged()
    {
        loader.setVisible(!hostProcessor.isPluginLoaded());
        closeButton.setVisible(hostProcessor.isPluginLoaded());

        if (hostProcessor.isPluginLoaded())
        {
            auto editorComponent = std::make_unique<PluginEditorComponent>(
                hostProcessor.createInnerEditor(),
                [this]
                {
                    [[maybe_unused]] const auto posted = MessageManager::callAsync([this] { clearPlugin(); });
                    jassert(posted);
                }
            );

            editorComponent->setScaleFactor(currentScaleFactor);
            currentEditorComponent = editorComponent.get();

            editor = [&]() -> std::unique_ptr<Component>
            {
                switch (hostProcessor.getEditorStyle())
                {
                case EditorStyle::thisWindow:
                    addAndMakeVisible(editorComponent.get());
                    setSize(editorComponent->getWidth(), editorComponent->getHeight());
                    return std::move(editorComponent);

                case EditorStyle::newWindow:
                    const auto bg = getLookAndFeel().findColour(ResizableWindow::backgroundColourId).darker();
                    auto window = std::make_unique<ScaledDocumentWindow>(bg, currentScaleFactor);
                    window->setAlwaysOnTop(true);
                    window->setContentOwned(editorComponent.release(), true);
                    window->centreAroundComponent(this, window->getWidth(), window->getHeight());
                    window->setVisible(true);
                    return window;
                }

                jassertfalse;
                return nullptr;
            }();
        }
        else
        {
            editor = nullptr;
            setSize(500, 500);
        }
    }

    void clearPlugin()
    {
        currentEditorComponent = nullptr;
        editor = nullptr;
        hostProcessor.clearPlugin();
    }

    static constexpr auto buttonHeight = 30;

    HostAudioProcessorImpl& hostProcessor;
    PluginLoaderComponent loader;
    std::unique_ptr<Component> editor;
    PluginEditorComponent* currentEditorComponent = nullptr;
    ScopedValueSetter<std::function<void()>> scopedCallback;
    TextButton closeButton{"Close Plugin"};
    float currentScaleFactor = 1.0f;

    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;
};

//==============================================================================
class HostAudioProcessor final : public HostAudioProcessorImpl
{
public:
    bool hasEditor() const override
    {
        return true;
    }

    AudioProcessorEditor* createEditor() override
    {
        return new HostAudioProcessorEditor(*this);
    }
};
