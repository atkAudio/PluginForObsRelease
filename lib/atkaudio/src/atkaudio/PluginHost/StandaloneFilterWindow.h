#pragma once
#include "JuceHostPlugin.h"

#ifndef DOXYGEN
#include <juce_audio_plugin_client/detail/juce_CreatePluginFilter.h>
#endif

class StandalonePluginHolder2
    // : private AudioIODeviceCallback
    : private Timer
    , private Value::Listener
{
public:
    //==============================================================================
    /** Structure used for the number of inputs and outputs. */
    struct PluginInOuts
    {
        short numIns, numOuts;
    };

    //==============================================================================
    /** Creates an instance of the default plugin.

        The settings object can be a PropertySet that the class should use to store its
        settings - the takeOwnershipOfSettings indicates whether this object will delete
        the settings automatically when no longer needed. The settings can also be nullptr.

        A default device name can be passed in.

        Preferably a complete setup options object can be used, which takes precedence over
        the preferredDefaultDeviceName and allows you to select the input & output device names,
        sample rate, buffer size etc.

        In all instances, the settingsToUse will take precedence over the "preferred" options if not null.
    */
    StandalonePluginHolder2(
        PropertySet* settingsToUse,
        bool takeOwnershipOfSettings = true,
        const String& preferredDefaultDeviceName = String(),
        const AudioDeviceManager::AudioDeviceSetup* preferredSetupOptions = nullptr,
        const Array<PluginInOuts>& channels = Array<PluginInOuts>(),
#if JUCE_ANDROID || JUCE_IOS
        bool shouldAutoOpenMidiDevices = true
#else
        bool shouldAutoOpenMidiDevices = false
#endif
    )

        : settings(settingsToUse, takeOwnershipOfSettings)
        , channelConfiguration(channels)
        , autoOpenMidiDevices(shouldAutoOpenMidiDevices)
    {
        // Only one StandalonePluginHolder2 may be created at a time
        jassert(currentInstance == nullptr);
        currentInstance = this;

        // shouldMuteInput.addListener(this);
        // shouldMuteInput = !isInterAppAudioConnected();

        handleCreatePlugin();

        auto inChannels =
            (channelConfiguration.size() > 0 ? channelConfiguration[0].numIns
                                             : processor->getMainBusNumInputChannels());

        if (preferredSetupOptions != nullptr)
            options.reset(new AudioDeviceManager::AudioDeviceSetup(*preferredSetupOptions));

        auto audioInputRequired = (inChannels > 0);

        if (audioInputRequired && RuntimePermissions::isRequired(RuntimePermissions::recordAudio)
            && !RuntimePermissions::isGranted(RuntimePermissions::recordAudio))
            RuntimePermissions::request(
                RuntimePermissions::recordAudio,
                [this, preferredDefaultDeviceName](bool granted) { init(granted, preferredDefaultDeviceName); }
            );
        else
            init(audioInputRequired, preferredDefaultDeviceName);
    }

    void init(bool enableAudioInput, const String& preferredDefaultDeviceName)
    {
        (void)enableAudioInput;
        (void)preferredDefaultDeviceName;
        // setupAudioDevices(enableAudioInput, preferredDefaultDeviceName, options.get());

        reloadPluginState();
        startPlaying();

        if (autoOpenMidiDevices)
            startTimer(500);
    }

    ~StandalonePluginHolder2() override
    {
        stopTimer();

        auto* hostAudioProcessor = dynamic_cast<HostAudioProcessor*>(processor.get());
        if (hostAudioProcessor != nullptr)
            hostAudioProcessor->clearPlugin();

        handleDeletePlugin();
        // shutDownAudioDevices();

        currentInstance = nullptr;
    }

    //==============================================================================
    virtual void createPlugin()
    {
        handleCreatePlugin();
    }

    virtual void deletePlugin()
    {
        handleDeletePlugin();
    }

    int getNumInputChannels() const
    {
        if (processor == nullptr)
            return 0;

        return (
            channelConfiguration.size() > 0 ? channelConfiguration[0].numIns : processor->getMainBusNumInputChannels()
        );
    }

    int getNumOutputChannels() const
    {
        if (processor == nullptr)
            return 0;

        return (
            channelConfiguration.size() > 0 ? channelConfiguration[0].numOuts : processor->getMainBusNumOutputChannels()
        );
    }

    static String getFilePatterns(const String& fileSuffix)
    {
        if (fileSuffix.isEmpty())
            return {};

        return (fileSuffix.startsWithChar('.') ? "*" : "*.") + fileSuffix;
    }

    //==============================================================================
    Value& getMuteInputValue()
    {
        return shouldMuteInput;
    }

    bool getProcessorHasPotentialFeedbackLoop() const
    {
        return processorHasPotentialFeedbackLoop;
    }

    void valueChanged(Value& value) override
    {
        muteInput = (bool)value.getValue();
    }

    //==============================================================================
    File getLastFile() const
    {
        File f;

        if (settings != nullptr)
            f = File(settings->getValue("lastStateFile"));

        if (f == File())
            f = File::getSpecialLocation(File::userDocumentsDirectory);

        return f;
    }

    void setLastFile(const FileChooser& fc)
    {
        if (settings != nullptr)
            settings->setValue("lastStateFile", fc.getResult().getFullPathName());
    }

    /** Pops up a dialog letting the user save the processor's state to a file. */
    void askUserToSaveState(const String& fileSuffix = String())
    {
        stateFileChooser =
            std::make_unique<FileChooser>(TRANS("Save current state"), getLastFile(), getFilePatterns(fileSuffix));
        auto flags = FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                   | FileBrowserComponent::warnAboutOverwriting;

        stateFileChooser->launchAsync(
            flags,
            [this](const FileChooser& fc)
            {
                if (fc.getResult() == File{})
                    return;

                setLastFile(fc);

                MemoryBlock data;
                processor->getStateInformation(data);

                if (!fc.getResult().replaceWithData(data.getData(), data.getSize()))
                {
                    auto opts = MessageBoxOptions::makeOptionsOk(
                        AlertWindow::WarningIcon,
                        TRANS("Error whilst saving"),
                        TRANS("Couldn't write to the specified file!")
                    );
                    messageBox = AlertWindow::showScopedAsync(opts, nullptr);
                }
            }
        );
    }

    /** Pops up a dialog letting the user re-load the processor's state from a file. */
    void askUserToLoadState(const String& fileSuffix = String())
    {
        stateFileChooser =
            std::make_unique<FileChooser>(TRANS("Load a saved state"), getLastFile(), getFilePatterns(fileSuffix));
        auto flags = FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles;

        stateFileChooser->launchAsync(
            flags,
            [this](const FileChooser& fc)
            {
                if (fc.getResult() == File{})
                    return;

                setLastFile(fc);

                MemoryBlock data;

                if (fc.getResult().loadFileAsData(data))
                {
                    processor->setStateInformation(data.getData(), (int)data.getSize());
                }
                else
                {
                    auto opts = MessageBoxOptions::makeOptionsOk(
                        AlertWindow::WarningIcon,
                        TRANS("Error whilst loading"),
                        TRANS("Couldn't read from the specified file!")
                    );
                    messageBox = AlertWindow::showScopedAsync(opts, nullptr);
                }
            }
        );
    }

    //==============================================================================
    void startPlaying()
    {
        // player.setProcessor(processor.get());

#if JucePlugin_Enable_IAA && JUCE_IOS
        if (auto device = dynamic_cast<iOSAudioIODevice*>(deviceManager.getCurrentAudioDevice()))
        {
            processor->setPlayHead(device->getAudioPlayHead());
            device->setMidiMessageCollector(&player.getMidiMessageCollector());
        }
#endif
    }

    void stopPlaying()
    {
        // player.setProcessor(nullptr);
    }

    //==============================================================================
    void savePluginState()
    {
        if (settings != nullptr && processor != nullptr)
        {
            MemoryBlock data;
            processor->getStateInformation(data);

            settings->setValue("filterState", data.toBase64Encoding());
        }
    }

    void reloadPluginState()
    {
        if (settings != nullptr)
        {
            MemoryBlock data;

            if (data.fromBase64Encoding(settings->getValue("filterState")) && data.getSize() > 0)
                processor->setStateInformation(data.getData(), (int)data.getSize());
        }
    }

    //==============================================================================
    void switchToHostApplication()
    {
#if JUCE_IOS
        if (auto device = dynamic_cast<iOSAudioIODevice*>(deviceManager.getCurrentAudioDevice()))
            device->switchApplication();
#endif
    }

    bool isInterAppAudioConnected()
    {
#if JUCE_IOS
        if (auto device = dynamic_cast<iOSAudioIODevice*>(deviceManager.getCurrentAudioDevice()))
            return device->isInterAppAudioConnected();
#endif

        return false;
    }

    Image getIAAHostIcon([[maybe_unused]] int size)
    {
#if JUCE_IOS && JucePlugin_Enable_IAA
        if (auto device = dynamic_cast<iOSAudioIODevice*>(deviceManager.getCurrentAudioDevice()))
            return device->getIcon(size);
#else
#endif

        return {};
    }

    // static StandalonePluginHolder2* getInstance()
    // {
    //     return currentInstance;
    // }

    //==============================================================================
    OptionalScopedPointer<PropertySet> settings;
    std::unique_ptr<AudioProcessor> processor;
    // AudioDeviceManager deviceManager;
    // AudioProcessorPlayer player;
    Array<PluginInOuts> channelConfiguration;

    // avoid feedback loop by default
    bool processorHasPotentialFeedbackLoop = true;
    std::atomic<bool> muteInput{true};
    Value shouldMuteInput;
    AudioBuffer<float> emptyBuffer;
    bool autoOpenMidiDevices;

    std::unique_ptr<AudioDeviceManager::AudioDeviceSetup> options;
    Array<MidiDeviceInfo> lastMidiDevices;

    std::unique_ptr<FileChooser> stateFileChooser;
    ScopedMessageBox messageBox;

private:
    StandalonePluginHolder2* currentInstance = nullptr;

    //==============================================================================
    void handleCreatePlugin()
    {
        processor = createPluginFilterOfType(AudioProcessor::wrapperType_Standalone);
        // processor->disableNonMainBuses();

        processor->setRateAndBufferSizeDetails(48000, 1024);

        processorHasPotentialFeedbackLoop = (getNumInputChannels() > 0 && getNumOutputChannels() > 0);
    }

    void handleDeletePlugin()
    {
        stopPlaying();
        processor = nullptr;
    }

    //==============================================================================
    /*  This class can be used to ensure that audio callbacks use buffers with a
        predictable maximum size.

        On some platforms (such as iOS 10), the expected buffer size reported in
        audioDeviceAboutToStart may be smaller than the blocks passed to
        audioDeviceIOCallbackWithContext. This can lead to out-of-bounds reads if the render
        callback depends on additional buffers which were initialised using the
        smaller size.

        As a workaround, this class will ensure that the render callback will
        only ever be called with a block with a length less than or equal to the
        expected block size.
    */
    class CallbackMaxSizeEnforcer : public AudioIODeviceCallback
    {
    public:
        explicit CallbackMaxSizeEnforcer(AudioIODeviceCallback& callbackIn)
            : inner(callbackIn)
        {
        }

        void audioDeviceAboutToStart(AudioIODevice* device) override
        {
            maximumSize = device->getCurrentBufferSizeSamples();
            storedInputChannels.resize((size_t)device->getActiveInputChannels().countNumberOfSetBits());
            storedOutputChannels.resize((size_t)device->getActiveOutputChannels().countNumberOfSetBits());

            inner.audioDeviceAboutToStart(device);
        }

        void audioDeviceIOCallbackWithContext(
            const float* const* inputChannelData,
            [[maybe_unused]] int numInputChannels,
            float* const* outputChannelData,
            [[maybe_unused]] int numOutputChannels,
            int numSamples,
            const AudioIODeviceCallbackContext& context
        ) override
        {
            jassert((int)storedInputChannels.size() == numInputChannels);
            jassert((int)storedOutputChannels.size() == numOutputChannels);

            int position = 0;

            while (position < numSamples)
            {
                const auto blockLength = jmin(maximumSize, numSamples - position);

                initChannelPointers(inputChannelData, storedInputChannels, position);
                initChannelPointers(outputChannelData, storedOutputChannels, position);

                inner.audioDeviceIOCallbackWithContext(
                    storedInputChannels.data(),
                    (int)storedInputChannels.size(),
                    storedOutputChannels.data(),
                    (int)storedOutputChannels.size(),
                    blockLength,
                    context
                );

                position += blockLength;
            }
        }

        void audioDeviceStopped() override
        {
            inner.audioDeviceStopped();
        }

    private:
        struct GetChannelWithOffset
        {
            int offset;

            template <typename Ptr>
            auto operator()(Ptr ptr) const noexcept -> Ptr
            {
                return ptr + offset;
            }
        };

        template <typename Ptr, typename Vector>
        void initChannelPointers(Ptr&& source, Vector&& target, int offset)
        {
            std::transform(source, source + target.size(), target.begin(), GetChannelWithOffset{offset});
        }

        AudioIODeviceCallback& inner;
        int maximumSize = 0;
        std::vector<const float*> storedInputChannels;
        std::vector<float*> storedOutputChannels;
    };

    // CallbackMaxSizeEnforcer maxSizeEnforcer{*this};

    //==============================================================================
    class SettingsComponent : public Component
    {
    public:
        SettingsComponent(
            StandalonePluginHolder2& pluginHolder,
            AudioDeviceManager& deviceManagerToUse,
            int maxAudioInputChannels,
            int maxAudioOutputChannels
        )
            : owner(pluginHolder)
            , deviceSelector(
                  deviceManagerToUse,
                  0,
                  maxAudioInputChannels,
                  0,
                  maxAudioOutputChannels,
                  true,
                  (pluginHolder.processor.get() != nullptr && pluginHolder.processor->producesMidi()),
                  true,
                  false
              )
            , shouldMuteLabel("Feedback Loop:", "Feedback Loop:")
            , shouldMuteButton("Mute audio input")
        {
            setOpaque(true);

            shouldMuteButton.setClickingTogglesState(true);
            shouldMuteButton.getToggleStateValue().referTo(owner.shouldMuteInput);

            addAndMakeVisible(deviceSelector);

            if (owner.getProcessorHasPotentialFeedbackLoop())
            {
                addAndMakeVisible(shouldMuteButton);
                addAndMakeVisible(shouldMuteLabel);

                shouldMuteLabel.attachToComponent(&shouldMuteButton, true);
            }
        }

        void paint(Graphics& g) override
        {
            g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));
        }

        void resized() override
        {
            const ScopedValueSetter<bool> scope(isResizing, true);

            auto r = getLocalBounds();

            if (owner.getProcessorHasPotentialFeedbackLoop())
            {
                auto itemHeight = deviceSelector.getItemHeight();
                auto extra = r.removeFromTop(itemHeight);

                auto seperatorHeight = (itemHeight >> 1);
                shouldMuteButton.setBounds(
                    Rectangle<int>(
                        extra.proportionOfWidth(0.35f),
                        seperatorHeight,
                        extra.proportionOfWidth(0.60f),
                        deviceSelector.getItemHeight()
                    )
                );

                r.removeFromTop(seperatorHeight);
            }

            deviceSelector.setBounds(r);
        }

        void childBoundsChanged(Component* childComp) override
        {
            if (!isResizing && childComp == &deviceSelector)
                setToRecommendedSize();
        }

        void setToRecommendedSize()
        {
            const auto extraHeight = [&]
            {
                if (!owner.getProcessorHasPotentialFeedbackLoop())
                    return 0;

                const auto itemHeight = deviceSelector.getItemHeight();
                const auto separatorHeight = (itemHeight >> 1);
                return itemHeight + separatorHeight;
            }();

            setSize(getWidth(), deviceSelector.getHeight() + extraHeight);
        }

    private:
        //==============================================================================
        StandalonePluginHolder2& owner;
        AudioDeviceSelectorComponent deviceSelector;
        Label shouldMuteLabel;
        ToggleButton shouldMuteButton;
        bool isResizing = false;

        //==============================================================================
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsComponent)
    };

    void timerCallback() override
    {
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StandalonePluginHolder2)
};

//==============================================================================
/**
    A class that can be used to run a simple standalone application containing your filter.

    Just create one of these objects in your JUCEApplicationBase::initialise() method, and
    let it do its work. It will create your filter object using the same createPluginFilter() function
    that the other plugin wrappers use.

    @tags{Audio}
*/
class StandaloneFilterWindow
    : public DocumentWindow
    , private Button::Listener
{
public:
    //==============================================================================
    typedef StandalonePluginHolder2::PluginInOuts PluginInOuts;

    StandaloneFilterWindow(
        const String& title,
        Colour backgroundColour,
        std::unique_ptr<StandalonePluginHolder2> pluginHolderIn
    )
        : DocumentWindow(title, backgroundColour, DocumentWindow::minimiseButton | DocumentWindow::closeButton, false)
        , pluginHolder(std::move(pluginHolderIn))
    {
        setConstrainer(&decoratorConstrainer);

        setTitleBarButtonsRequired(DocumentWindow::minimiseButton | DocumentWindow::closeButton, false);

        updateContent();

        const auto windowScreenBounds = [this]() -> Rectangle<int>
        {
            const auto width = getWidth();
            const auto height = getHeight();

            const auto& displays = Desktop::getInstance().getDisplays();

            if (displays.displays.isEmpty())
                return {width, height};

            if (auto* props = pluginHolder->settings.get())
            {
                constexpr int defaultValue = -100;

                const auto x = props->getIntValue("windowX", defaultValue);
                const auto y = props->getIntValue("windowY", defaultValue);

                if (x != defaultValue && y != defaultValue)
                {
                    const auto screenLimits = displays.getDisplayForRect({x, y, width, height})->userArea;

                    return {
                        jlimit(screenLimits.getX(), jmax(screenLimits.getX(), screenLimits.getRight() - width), x),
                        jlimit(screenLimits.getY(), jmax(screenLimits.getY(), screenLimits.getBottom() - height), y),
                        width,
                        height
                    };
                }
            }

            const auto displayArea = displays.getPrimaryDisplay()->userArea;

            return {displayArea.getCentreX() - width / 2, displayArea.getCentreY() - height / 2, width, height};
        }();

        setBoundsConstrained(windowScreenBounds);

        if (auto* processor = getAudioProcessor())
            if (auto* editor = processor->getActiveEditor())
                setResizable(editor->isResizable(), false);
    }

    ~StandaloneFilterWindow() override
    {
#if (!JUCE_IOS) && (!JUCE_ANDROID)
        if (auto* props = pluginHolder->settings.get())
        {
            props->setValue("windowX", getX());
            props->setValue("windowY", getY());
        }
#endif

        pluginHolder->stopPlaying();
        clearContentComponent();
        pluginHolder = nullptr;
    }

    void visibilityChanged() override
    {
        clearContentComponent();
        if (isVisible())
            updateContent();
    }

    //==============================================================================
    AudioProcessor* getAudioProcessor() const noexcept
    {
        return pluginHolder->processor.get();
    }

    // AudioDeviceManager& getDeviceManager() const noexcept
    // {
    //     return pluginHolder->deviceManager;
    // }
    juce::CriticalSection pluginHolderLock;

    auto& getPluginHolderLock()
    {
        return pluginHolderLock;
    }

    /** Deletes and re-creates the plugin, resetting it to its default state. */
    void resetToDefaultState()
    {
        juce::ScopedLock lock(pluginHolderLock);
        pluginHolder->stopPlaying();
        clearContentComponent();
        pluginHolder->deletePlugin();

        if (auto* props = pluginHolder->settings.get())
            props->removeValue("filterState");

        pluginHolder->createPlugin();
        updateContent();
        pluginHolder->startPlaying();
    }

    //==============================================================================
    void closeButtonPressed() override
    {
        setVisible(false);
    }

    void obsPluginShutdown()
    {
        pluginHolder->savePluginState();
    }

    void handleMenuResult(int result)
    {
        (void)result;
    }

    static void menuCallback(int result, StandaloneFilterWindow* button)
    {
        if (button != nullptr && result != 0)
            button->handleMenuResult(result);
    }

    void resized() override
    {
        DocumentWindow::resized();
        // optionsButton.setBounds(8, 6, 60, getTitleBarHeight() - 8);
    }

    virtual StandalonePluginHolder2* getPluginHolder()
    {
        return pluginHolder.get();
    }

    std::unique_ptr<StandalonePluginHolder2> pluginHolder;

private:
    void updateContent()
    {
        auto* content = new MainContentComponent(*this);
        decoratorConstrainer.setMainContentComponent(content);

        constexpr auto resizeAutomatically = true;

        setContentOwned(content, resizeAutomatically);
    }

    void buttonClicked(Button*) override
    {
        PopupMenu m;
        m.addItem(1, TRANS("Audio/MIDI Settings..."));
        m.addSeparator();
        m.addItem(2, TRANS("Save current state..."));
        m.addItem(3, TRANS("Load a saved state..."));
        m.addSeparator();
        m.addItem(4, TRANS("Reset to default state"));

        m.showMenuAsync(PopupMenu::Options(), ModalCallbackFunction::forComponent(menuCallback, this));
    }

    //==============================================================================
    class MainContentComponent
        : public Component
        , private Value::Listener
        , private Button::Listener
        , private ComponentListener
    {
    public:
        MainContentComponent(StandaloneFilterWindow& filterWindow)
            : owner(filterWindow)
            // , notification(this)
            , editor(
                  owner.getAudioProcessor()->hasEditor() ? owner.getAudioProcessor()->createEditorIfNeeded()
                                                         : new GenericAudioProcessorEditor(*owner.getAudioProcessor())
              )
        {
            inputMutedValue.referTo(owner.pluginHolder->getMuteInputValue());

            if (editor != nullptr)
            {
                editor->addComponentListener(this);
                handleMovedOrResized();

                addAndMakeVisible(editor.get());
            }

            // addChildComponent(notification);

            if (owner.pluginHolder->getProcessorHasPotentialFeedbackLoop())
            {
                inputMutedValue.addListener(this);
                shouldShowNotification = inputMutedValue.getValue();
            }

            inputMutedChanged(shouldShowNotification);
        }

        ~MainContentComponent() override
        {
            if (editor != nullptr)
            {
                editor->removeComponentListener(this);
                owner.pluginHolder->processor->editorBeingDeleted(editor.get());
                editor = nullptr;
            }
        }

        void resized() override
        {
            handleResized();
        }

        ComponentBoundsConstrainer* getEditorConstrainer() const
        {
            if (auto* e = editor.get())
                return e->getConstrainer();

            return nullptr;
        }

        BorderSize<int> computeBorder() const
        {
            const auto nativeFrame = [&]() -> BorderSize<int>
            {
                if (auto* peer = owner.getPeer())
                    if (const auto frameSize = peer->getFrameSizeIfPresent())
                        return *frameSize;

                return {};
            }();

            return nativeFrame.addedTo(owner.getContentComponentBorder())
                .addedTo(BorderSize<int>{shouldShowNotification ? NotificationArea::height : 0, 0, 0, 0});
        }

    private:
        //==============================================================================
        class NotificationArea : public Component
        {
        public:
            enum
            {
                height = 30
            };

            NotificationArea(Button::Listener* settingsButtonListener)
                : notification("notification", "Audio input is muted to avoid feedback loop")
                ,
#if JUCE_IOS || JUCE_ANDROID
                settingsButton("Unmute Input")
#else
                settingsButton("Settings...")
#endif
            {
                setOpaque(true);

                notification.setColour(Label::textColourId, Colours::black);

                settingsButton.addListener(settingsButtonListener);

                addAndMakeVisible(notification);
                addAndMakeVisible(settingsButton);
            }

            void paint(Graphics& g) override
            {
                auto r = getLocalBounds();

                g.setColour(Colours::darkgoldenrod);
                g.fillRect(r.removeFromBottom(1));

                g.setColour(Colours::lightgoldenrodyellow);
                g.fillRect(r);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced(5);

                settingsButton.setBounds(r.removeFromRight(70));
                notification.setBounds(r);
            }

        private:
            Label notification;
            TextButton settingsButton;
        };

        //==============================================================================
        void inputMutedChanged(bool newInputMutedValue)
        {
            shouldShowNotification = newInputMutedValue;
            // notification.setVisible(shouldShowNotification);

#if JUCE_IOS || JUCE_ANDROID
            handleResized();
#else
            if (editor != nullptr)
            {
                const int extraHeight = shouldShowNotification ? NotificationArea::height : 0;
                const auto rect = getSizeToContainEditor();
                setSize(rect.getWidth(), rect.getHeight() + extraHeight);
            }
#endif
        }

        void valueChanged(Value& value) override
        {
            inputMutedChanged(value.getValue());
        }

        void buttonClicked(Button*) override
        {
        }

        //==============================================================================
        void handleResized()
        {
            auto r = getLocalBounds();

            if (editor != nullptr)
            {
                const auto newPos = r.getTopLeft().toFloat().transformedBy(editor->getTransform().inverted());

                if (preventResizingEditor)
                    editor->setTopLeftPosition(newPos.roundToInt());
                else
                    editor->setBoundsConstrained(
                        editor->getLocalArea(this, r.toFloat()).withPosition(newPos).toNearestInt()
                    );
            }
        }

        void handleMovedOrResized()
        {
            const ScopedValueSetter<bool> scope(preventResizingEditor, true);

            if (editor != nullptr)
            {
                auto rect = getSizeToContainEditor();

                setSize(rect.getWidth(), rect.getHeight() + (shouldShowNotification ? NotificationArea::height : 0));
            }
        }

        void componentMovedOrResized(Component&, bool, bool) override
        {
            handleMovedOrResized();
        }

        Rectangle<int> getSizeToContainEditor() const
        {
            if (editor != nullptr)
                return getLocalArea(editor.get(), editor->getLocalBounds());

            return {};
        }

        //==============================================================================
        StandaloneFilterWindow& owner;
        // NotificationArea notification;
        std::unique_ptr<AudioProcessorEditor> editor;
        Value inputMutedValue;
        bool shouldShowNotification = false;
        bool preventResizingEditor = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainContentComponent)
    };

    /*  This custom constrainer checks with the AudioProcessorEditor (which might itself be
        constrained) to ensure that any size we choose for the standalone window will be suitable
        for the editor too.

        Without this constrainer, attempting to resize the standalone window may set bounds on the
        peer that are unsupported by the inner editor. In this scenario, the peer will be set to a
        'bad' size, then the inner editor will be resized. The editor will check the new bounds with
        its own constrainer, and may set itself to a more suitable size. After that, the resizable
        window will see that its content component has changed size, and set the bounds of the peer
        accordingly. The end result is that the peer is resized twice in a row to different sizes,
        which can appear glitchy/flickery to the user.
    */
    class DecoratorConstrainer : public BorderedComponentBoundsConstrainer
    {
    public:
        ComponentBoundsConstrainer* getWrappedConstrainer() const override
        {
            return contentComponent != nullptr ? contentComponent->getEditorConstrainer() : nullptr;
        }

        BorderSize<int> getAdditionalBorder() const override
        {
            return contentComponent != nullptr ? contentComponent->computeBorder() : BorderSize<int>{};
        }

        void setMainContentComponent(MainContentComponent* in)
        {
            contentComponent = in;
        }

    private:
        MainContentComponent* contentComponent = nullptr;
    };

    //==============================================================================
    // TextButton optionsButton;
    DecoratorConstrainer decoratorConstrainer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StandaloneFilterWindow)
};

// } // namespace juce
