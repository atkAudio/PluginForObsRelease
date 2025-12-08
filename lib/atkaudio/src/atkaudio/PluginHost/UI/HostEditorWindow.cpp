#include "HostEditorWindow.h"

#include <atkaudio/SharedPluginList.h>
#include <juce_gui_extra/juce_gui_extra.h>

using namespace juce;

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
    setResizable(true, false);
    addAndMakeVisible(loader);

    hostProcessor.pluginChanged();
}

void HostAudioProcessorEditor::paint(Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId).darker());
}

void HostAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    loader.setBounds(bounds);

    // When we're resized externally, resize the editor to match
    if (editor != nullptr && !resizingFromChild)
        editor->setBounds(bounds);
}

void HostAudioProcessorEditor::childBoundsChanged(Component* child)
{
    if (child != editor.get())
        return;

    const auto size = editor != nullptr ? editor->getLocalBounds() : Rectangle<int>();

    resizingFromChild = true;
    setSize(size.getWidth(), size.getHeight());
    resizingFromChild = false;
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
        editorComponent->setFooterVisible(pendingFooterVisible);
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

void HostAudioProcessorEditor::setFooterVisible(bool visible)
{
    pendingFooterVisible = visible;
    if (currentEditorComponent != nullptr)
        currentEditorComponent->setFooterVisible(visible);
}

ComponentBoundsConstrainer* HostAudioProcessorEditor::getPluginConstrainer() const
{
    if (currentEditorComponent != nullptr)
        return currentEditorComponent->getEditorConstrainer();
    return nullptr;
}

//==============================================================================
// HostEditorComponent - Main content for Qt embedding
//==============================================================================

class HostEditorComponent::MainContentComponent
    : public Component
    , private Value::Listener
    , private ComponentListener
{
public:
    MainContentComponent(HostEditorComponent& ownerComponent)
        : owner(ownerComponent)
        , editor(
              owner.getAudioProcessor()->hasEditor() ? owner.getAudioProcessor()->createEditorIfNeeded()
                                                     : new GenericAudioProcessorEditor(*owner.getAudioProcessor())
          )
    {
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
        g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        handleResized();
    }

    ComponentBoundsConstrainer* getEditorConstrainer() const
    {
        if (auto* hostEditor = dynamic_cast<HostAudioProcessorEditor*>(editor.get()))
            return hostEditor->getPluginConstrainer();
        return nullptr;
    }

    AudioProcessorEditor* getEditor() const
    {
        return editor.get();
    }

    void setFooterVisible(bool visible)
    {
        if (auto* hostEditor = dynamic_cast<HostAudioProcessorEditor*>(editor.get()))
            hostEditor->setFooterVisible(visible);
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

    void handleResized()
    {
        auto r = getLocalBounds();

        if (editor != nullptr)
        {
            if (preventResizingEditor)
            {
                const auto newPos = r.getTopLeft().toFloat().transformedBy(editor->getTransform().inverted());
                editor->setTopLeftPosition(newPos.roundToInt());
            }
            else
            {
                editor->setBounds(r);
            }
        }
    }

    void handleMovedOrResized()
    {
        const ScopedValueSetter<bool> scope(preventResizingEditor, true);

        if (editor != nullptr)
        {
            auto rect = getSizeToContainEditor();
            setSize(rect.getWidth(), rect.getHeight());

            if (auto* parent = getParentComponent())
                parent->setSize(rect.getWidth(), rect.getHeight());
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

    HostEditorComponent& owner;
    std::unique_ptr<AudioProcessorEditor> editor;
    Value inputMutedValue;
    bool shouldShowNotification = false;
    bool preventResizingEditor = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainContentComponent)
};

HostEditorComponent::HostEditorComponent(std::unique_ptr<PluginHolder> pluginHolderIn)
    : pluginHolder(std::move(pluginHolderIn))
{
    setOpaque(true);
    updateContent();

    editorToWatch = contentComponent->getEditor();
    if (editorToWatch != nullptr)
        editorToWatch->addComponentListener(this);

    setSize(contentComponent->getWidth(), contentComponent->getHeight());

    if (getWidth() == 0 || getHeight() == 0)
        setSize(500, 500);
}

HostEditorComponent::~HostEditorComponent()
{
    if (editorToWatch != nullptr)
        editorToWatch->removeComponentListener(this);

    pluginHolder->stopPlaying();
    contentComponent = nullptr;
    pluginHolder = nullptr;
}

void HostEditorComponent::paint(Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));
}

void HostEditorComponent::resized()
{
    if (contentComponent != nullptr && !resizingFromEditor)
        contentComponent->setBounds(getLocalBounds());
}

void HostEditorComponent::childBoundsChanged(Component* child)
{
    if (child == contentComponent.get() && contentComponent != nullptr)
    {
        const auto newWidth = contentComponent->getWidth();
        const auto newHeight = contentComponent->getHeight();

        if (newWidth > 0 && newHeight > 0 && (newWidth != getWidth() || newHeight != getHeight()))
            setSize(newWidth, newHeight);
    }
}

void HostEditorComponent::componentMovedOrResized(Component& component, bool /*wasMoved*/, bool wasResized)
{
    if (wasResized && &component == editorToWatch)
    {
        const auto newWidth = component.getWidth();
        const auto newHeight = component.getHeight();

        if (newWidth > 0 && newHeight > 0)
        {
            resizingFromEditor = true;
            setSize(newWidth, newHeight);
            resizingFromEditor = false;
        }
    }
}

AudioProcessor* HostEditorComponent::getAudioProcessor() const noexcept
{
    return pluginHolder ? pluginHolder->processor.get() : nullptr;
}

HostAudioProcessorImpl* HostEditorComponent::getHostProcessor() const noexcept
{
    return pluginHolder ? pluginHolder->getHostProcessor() : nullptr;
}

CriticalSection& HostEditorComponent::getPluginHolderLock()
{
    return pluginHolderLock;
}

PluginHolder* HostEditorComponent::getPluginHolder()
{
    return pluginHolder.get();
}

ComponentBoundsConstrainer* HostEditorComponent::getEditorConstrainer() const
{
    if (contentComponent != nullptr)
        return contentComponent->getEditorConstrainer();
    return nullptr;
}

void HostEditorComponent::setFooterVisible(bool visible)
{
    if (contentComponent != nullptr)
        contentComponent->setFooterVisible(visible);
}

void HostEditorComponent::destroyUI()
{
    if (editorToWatch != nullptr)
    {
        editorToWatch->removeComponentListener(this);
        editorToWatch = nullptr;
    }

    contentComponent = nullptr;
}

void HostEditorComponent::recreateUI()
{
    if (!pluginHolder || !pluginHolder->processor)
        return;

    destroyUI();
    updateContent();

    bool isDocked = getIsDocked ? getIsDocked() : false;
    contentComponent->setFooterVisible(!isDocked);

    editorToWatch = contentComponent->getEditor();
    if (editorToWatch != nullptr)
        editorToWatch->addComponentListener(this);

    setSize(contentComponent->getWidth(), contentComponent->getHeight());
}

void HostEditorComponent::updateContent()
{
    contentComponent = std::make_unique<MainContentComponent>(*this);
    addAndMakeVisible(contentComponent.get());

    if (contentComponent != nullptr)
        setSize(contentComponent->getWidth(), contentComponent->getHeight());
}
