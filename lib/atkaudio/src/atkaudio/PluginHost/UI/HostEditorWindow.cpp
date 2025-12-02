#include "HostEditorWindow.h"

#include <atkaudio/SharedPluginList.h>
#include <juce_gui_extra/juce_gui_extra.h>

using namespace juce;

//==============================================================================
// HostAudioProcessorEditor implementation
//==============================================================================
HostAudioProcessorEditor::HostAudioProcessorEditor(HostAudioProcessorImpl& owner)
    : AudioProcessorEditor(owner)
    , hostProcessor(owner)
    , loader(
          owner.pluginFormatManager,
          owner.pluginList,
          atk::SharedPluginList::getInstance()->getPropertiesFile(),
          &owner,
          [&owner](const PluginDescription& pd, EditorStyle editorStyle) { owner.setNewPlugin(pd, editorStyle); }
      )
    , scopedCallback(owner.pluginChanged, [this] { pluginChanged(); })
{
    setSize(500, 500);
    setResizable(false, false);
    addAndMakeVisible(loader);

    hostProcessor.pluginChanged();
}

void HostAudioProcessorEditor::parentSizeChanged()
{
}

void HostAudioProcessorEditor::paint(Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId).darker());
}

void HostAudioProcessorEditor::resized()
{
    loader.setBounds(getLocalBounds());
}

void HostAudioProcessorEditor::childBoundsChanged(Component* child)
{
    if (child != editor.get())
        return;

    const auto size = editor != nullptr ? editor->getLocalBounds() : Rectangle<int>();
    setSize(size.getWidth(), size.getHeight());
}

void HostAudioProcessorEditor::setScaleFactor(float scale)
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

void HostAudioProcessorEditor::pluginChanged()
{
    loader.setVisible(!hostProcessor.isPluginLoaded());

    if (hostProcessor.isPluginLoaded())
    {
        auto editorComponent = std::make_unique<PluginEditorComponent>(
            hostProcessor.createInnerEditor(),
            &hostProcessor,
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

void HostAudioProcessorEditor::clearPlugin()
{
    currentEditorComponent = nullptr;
    editor = nullptr;
    hostProcessor.clearPlugin();
}

//==============================================================================
// HostEditorWindow implementation
//==============================================================================
class HostEditorWindow::MainContentComponent
    : public Component
    , private Value::Listener
    , private Button::Listener
    , private ComponentListener
{
public:
    MainContentComponent(HostEditorWindow& filterWindow)
        : owner(filterWindow)
        , editor(
              owner.getAudioProcessor()->hasEditor() ? owner.getAudioProcessor()->createEditorIfNeeded()
                                                     : new GenericAudioProcessorEditor(*owner.getAudioProcessor())
          )
    {
        // Ensure the component is opaque to prevent transparency/z-order issues
        setOpaque(true);

        inputMutedValue.referTo(owner.pluginHolder->getMuteInputValue());

        if (editor != nullptr)
        {
            editor->addComponentListener(this);
            handleMovedOrResized();
            addAndMakeVisible(editor.get());
        }

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

    void paint(Graphics& g) override
    {
        g.fillAll(owner.getBackgroundColour());
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

        return nativeFrame.addedTo(owner.getContentComponentBorder()).addedTo(BorderSize<int>{0, 0, 0, 0});
    }

private:
    void inputMutedChanged(bool newInputMutedValue)
    {
        shouldShowNotification = newInputMutedValue;

        if (editor != nullptr)
        {
            const auto rect = getSizeToContainEditor();
            setSize(rect.getWidth(), rect.getHeight());
        }
    }

    void valueChanged(Value& value) override
    {
        inputMutedChanged(value.getValue());
    }

    void buttonClicked(Button* button) override
    {
        // Unused - no buttons in MainContentComponent
        (void)button;
    }

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
            setSize(rect.getWidth(), rect.getHeight());
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

    HostEditorWindow& owner;
    std::unique_ptr<AudioProcessorEditor> editor;
    Value inputMutedValue;
    bool shouldShowNotification = false;
    bool preventResizingEditor = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainContentComponent)
};

class HostEditorWindow::DecoratorConstrainer : public BorderedComponentBoundsConstrainer
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

HostEditorWindow::HostEditorWindow(
    const String& title,
    Colour backgroundColour,
    std::unique_ptr<PluginHolder> pluginHolderIn,
    std::function<bool()> getMultiCoreEnabledCallback,
    std::function<void(bool)> setMultiCoreEnabledCallback
)
    : juce::DocumentWindow(title, backgroundColour, DocumentWindow::allButtons)
    , pluginHolder(std::move(pluginHolderIn))
    , decoratorConstrainer(new DecoratorConstrainer())
    , getMultiCoreEnabled(getMultiCoreEnabledCallback)
    , setMultiCoreEnabled(setMultiCoreEnabledCallback)
{
    setConstrainer(decoratorConstrainer);
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

    // Window starts off-desktop - AudioModule::setVisible() will add to desktop and show
    removeFromDesktop();
}

HostEditorWindow::~HostEditorWindow()
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
    delete decoratorConstrainer;
}

void HostEditorWindow::visibilityChanged()
{
    // Empty - content is created once in constructor
}

AudioProcessor* HostEditorWindow::getAudioProcessor() const noexcept
{
    return pluginHolder ? pluginHolder->processor.get() : nullptr;
}

HostAudioProcessorImpl* HostEditorWindow::getHostProcessor() const noexcept
{
    return pluginHolder ? pluginHolder->getHostProcessor() : nullptr;
}

CriticalSection& HostEditorWindow::getPluginHolderLock()
{
    return pluginHolderLock;
}

void HostEditorWindow::resetToDefaultState()
{
    ScopedLock lock(pluginHolderLock);
    pluginHolder->stopPlaying();
    clearContentComponent();
    pluginHolder->deletePlugin();

    if (auto* props = pluginHolder->settings.get())
        props->removeValue("filterState");

    pluginHolder->createPlugin();
    updateContent();
    pluginHolder->startPlaying();
}

void HostEditorWindow::closeButtonPressed()
{
    setVisible(false);
}

void HostEditorWindow::obsPluginShutdown()
{
    pluginHolder->savePluginState();
}

void HostEditorWindow::handleMenuResult(int result)
{
    (void)result;
}

void HostEditorWindow::menuCallback(int result, HostEditorWindow* button)
{
    if (button != nullptr && result != 0)
        button->handleMenuResult(result);
}

void HostEditorWindow::resized()
{
    DocumentWindow::resized();
}

PluginHolder* HostEditorWindow::getPluginHolder()
{
    return pluginHolder.get();
}

void HostEditorWindow::updateContent()
{
    auto* content = new MainContentComponent(*this);
    decoratorConstrainer->setMainContentComponent(content);

    constexpr auto resizeAutomatically = true;
    setContentOwned(content, resizeAutomatically);
}

void HostEditorWindow::buttonClicked(Button*)
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
