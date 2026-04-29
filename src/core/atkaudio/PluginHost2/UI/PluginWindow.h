#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

#include "../Core/ARAPlugin.h"

#include "IOConfigurationWindow.h"

#include <atkaudio/atkaudio.h>

inline String getFormatSuffix(const AudioProcessor* plugin)
{
    const auto format = [plugin]()
    {
        juce::String s;
        if (auto* instance = dynamic_cast<const AudioPluginInstance*>(plugin))
            s = instance->getPluginDescription().pluginFormatName;

        if (s == "Internal")
            s.clear();

        return s;
    }();

    return format.isNotEmpty() ? (" (" + format + ")") : format;
}

class PluginGraph;

class PluginDebugWindow final
    : public AudioProcessorEditor
    , public AudioProcessorParameter::Listener
    , public ListBoxModel
    , public AsyncUpdater
{
public:
    PluginDebugWindow(AudioProcessor& proc)
        : AudioProcessorEditor(proc)
        , audioProc(proc)
    {
        setSize(500, 200);
        addAndMakeVisible(list);

        for (auto* p : audioProc.getParameters())
            p->addListener(this);

        log.add("Parameter debug log started");
    }

    ~PluginDebugWindow() override
    {
        for (auto* p : audioProc.getParameters())
            p->removeListener(this);
    }

    void parameterValueChanged(int parameterIndex, float newValue) override
    {
        auto* param = audioProc.getParameters()[parameterIndex];
        auto value = param->getCurrentValueAsText().quoted() + " (" + String(newValue, 4) + ")";

        appendToLog("parameter change", *param, value);
    }

    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override
    {
        auto* param = audioProc.getParameters()[parameterIndex];
        appendToLog("gesture", *param, gestureIsStarting ? "start" : "end");
    }

private:
    void appendToLog(StringRef action, AudioProcessorParameter& param, StringRef value)
    {
        String entry(
            action + " " + param.getName(30).quoted() + " [" + String(param.getParameterIndex()) + "]: " + value
        );

        {
            ScopedLock lock(pendingLogLock);
            pendingLogEntries.add(entry);
        }

        triggerAsyncUpdate();
    }

    void resized() override
    {
        list.setBounds(getLocalBounds());
    }

    int getNumRows() override
    {
        return log.size();
    }

    void paintListBoxItem(int rowNumber, Graphics& g, int width, int height, bool) override
    {
        g.setColour(getLookAndFeel().findColour(TextEditor::textColourId));

        if (isPositiveAndBelow(rowNumber, log.size()))
            g.drawText(log[rowNumber], juce::Rectangle<int>{0, 0, width, height}, Justification::left, true);
    }

    void handleAsyncUpdate() override
    {
        if (log.size() > logSizeTrimThreshold)
            log.removeRange(0, log.size() - maxLogSize);

        {
            ScopedLock lock(pendingLogLock);
            log.addArray(pendingLogEntries);
            pendingLogEntries.clear();
        }

        list.updateContent();
        list.scrollToEnsureRowIsOnscreen(log.size() - 1);
    }

    constexpr static const int maxLogSize = 300;
    constexpr static const int logSizeTrimThreshold = 400;

    ListBox list{"Log", this};

    StringArray log;
    StringArray pendingLogEntries;
    CriticalSection pendingLogLock;

    AudioProcessor& audioProc;
};

class PluginWindow final : public juce::DocumentWindow
{
public:
    enum class Type
    {
        normal = 0,
        generic,
        programs,
        audioIO,
        debug,
        araHost,
        numTypes
    };

    PluginWindow(AudioProcessorGraphMT::Node* n, Type t, OwnedArray<PluginWindow>& windowList)
        : juce::DocumentWindow(
              n->getProcessor()->getName() + getFormatSuffix(n->getProcessor()),
              LookAndFeel::getDefaultLookAndFeel().findColour(ResizableWindow::backgroundColourId),
              DocumentWindow::allButtons
          )
        , activeWindowList(windowList)
        , node(n)
        , type(t)
    {
        setTitleBarButtonsRequired(DocumentWindow::minimiseButton | DocumentWindow::closeButton, false);
        setSize(400, 300);

        if (auto* ui = createProcessorEditor(*node->getProcessor(), type))
        {
            setContentOwned(ui, true);
            setResizable(ui->isResizable(), false);
        }

        setConstrainer(&constrainer);

#if JUCE_IOS || JUCE_ANDROID
        const auto screenBounds = Desktop::getInstance().getDisplays().getTotalBounds(true).toFloat();
        const auto scaleFactor = jmin(
            (screenBounds.getWidth() - 50.0f) / (float)getWidth(),
            (screenBounds.getHeight() - 50.0f) / (float)getHeight()
        );

        if (scaleFactor < 1.0f)
            setSize((int)(scaleFactor * (float)getWidth()), (int)(scaleFactor * (float)getHeight()));

        setTopLeftPosition(20, 20);
#else
        setTopLeftPosition(
            node->properties.getWithDefault(getLastXProp(type), Random::getSystemRandom().nextInt(500)),
            node->properties.getWithDefault(getLastYProp(type), Random::getSystemRandom().nextInt(500))
        );
#endif

        node->properties.set(getOpenProp(type), true);

        setVisible(true);
    }

    ~PluginWindow() override
    {
        // For normal editor windows, we need to notify the processor that
        // its editor is being deleted before actually deleting it.
        // This is required because AudioProcessorEditor's destructor asserts
        // that editorBeingDeleted() was called beforehand.
        if (type == Type::normal)
        {
            if (auto* editor = dynamic_cast<AudioProcessorEditor*>(getContentComponent()))
            {
                // The editor knows which processor it belongs to (could be an inner
                // processor in the case of InternalPlugin wrappers)
                editor->getAudioProcessor()->editorBeingDeleted(editor);

                // For InternalPlugin wrappers, we also need to clear the wrapper's
                // activeEditor since createEditorIfNeeded() was called on the wrapper
                // which then delegated to inner->createEditorIfNeeded()
                if (auto* wrapperProcessor = node->getProcessor())
                    if (wrapperProcessor->getActiveEditor() == editor)
                        wrapperProcessor->editorBeingDeleted(editor);
            }
        }
        clearContentComponent();
    }

    void moved() override
    {
        node->properties.set(getLastXProp(type), getX());
        node->properties.set(getLastYProp(type), getY());
    }

    void closeButtonPressed() override
    {
        node->properties.set(getOpenProp(type), false);
        activeWindowList.removeObject(this);
    }

    static String getLastXProp(Type type)
    {
        return "uiLastX_" + getTypeName(type);
    }

    static String getLastYProp(Type type)
    {
        return "uiLastY_" + getTypeName(type);
    }

    static String getOpenProp(Type type)
    {
        return "uiopen_" + getTypeName(type);
    }

    OwnedArray<PluginWindow>& activeWindowList;
    const AudioProcessorGraphMT::Node::Ptr node;
    const Type type;

    BorderSize<int> getBorderThickness() const override
    {
#if JUCE_IOS || JUCE_ANDROID
        const int border = 10;
        return {border, border, border, border};
#else
        return DocumentWindow::getBorderThickness();
#endif
    }

private:
    class DecoratorConstrainer final : public BorderedComponentBoundsConstrainer
    {
    public:
        explicit DecoratorConstrainer(DocumentWindow& windowIn)
            : window(windowIn)
        {
        }

        ComponentBoundsConstrainer* getWrappedConstrainer() const override
        {
            auto* editor = dynamic_cast<AudioProcessorEditor*>(window.getContentComponent());
            return editor != nullptr ? editor->getConstrainer() : nullptr;
        }

        BorderSize<int> getAdditionalBorder() const override
        {
            const auto nativeFrame = [&]() -> BorderSize<int>
            {
                if (auto* peer = window.getPeer())
                    if (const auto frameSize = peer->getFrameSizeIfPresent())
                        return *frameSize;

                return {};
            }();

            return nativeFrame.addedTo(window.getContentComponentBorder());
        }

    private:
        DocumentWindow& window;
    };

    DecoratorConstrainer constrainer{*this};

    static AudioProcessorEditor* createProcessorEditor(AudioProcessor& processor, PluginWindow::Type type)
    {
        if (type == PluginWindow::Type::normal)
        {
            if (processor.hasEditor())
                if (auto* ui = processor.createEditorIfNeeded())
                    return ui;

            type = PluginWindow::Type::generic;
        }

        if (type == PluginWindow::Type::araHost)
        {
#if JUCE_PLUGINHOST_ARA && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)
            if (auto* araPluginInstanceWrapper = dynamic_cast<ARAPluginInstanceWrapper*>(&processor))
                if (auto* ui = araPluginInstanceWrapper->createARAHostEditor())
                    return ui;
#endif
            return {};
        }

        if (type == PluginWindow::Type::generic)
        {
            auto* result = new GenericAudioProcessorEditor(processor);
            result->setResizeLimits(200, 300, 1'000, 10'000);
            return result;
        }

        if (type == PluginWindow::Type::programs)
            return new ProgramAudioProcessorEditor(processor);

        if (type == PluginWindow::Type::audioIO)
            return new IOConfigurationWindow(processor);

        if (type == PluginWindow::Type::debug)
            return new PluginDebugWindow(processor);

        jassertfalse;
        return {};
    }

    static String getTypeName(Type type)
    {
        switch (type)
        {
        case Type::normal:
            return "Normal";
        case Type::generic:
            return "Generic";
        case Type::programs:
            return "Programs";
        case Type::audioIO:
            return "IO";
        case Type::debug:
            return "Debug";
        case Type::araHost:
            return "ARAHost";
        case Type::numTypes:
        default:
            return {};
        }
    }

    struct ProgramAudioProcessorEditor final : public AudioProcessorEditor
    {
        explicit ProgramAudioProcessorEditor(AudioProcessor& p)
            : AudioProcessorEditor(p)
        {
            setOpaque(true);

            addAndMakeVisible(listBox);
            listBox.updateContent();

            const auto rowHeight = listBox.getRowHeight();

            setSize(400, jlimit(rowHeight, 400, p.getNumPrograms() * rowHeight));
        }

        void paint(Graphics& g) override
        {
            g.fillAll(Colours::grey);
        }

        void resized() override
        {
            listBox.setBounds(getLocalBounds());
        }

    private:
        class Model : public ListBoxModel
        {
        public:
            Model(Component& o, AudioProcessor& p)
                : owner(o)
                , proc(p)
            {
            }

            int getNumRows() override
            {
                return proc.getNumPrograms();
            }

            void paintListBoxItem(int rowNumber, Graphics& g, int width, int height, bool rowIsSelected) override
            {
                const auto textColour = owner.findColour(ListBox::textColourId);

                if (rowIsSelected)
                {
                    const auto defaultColour = owner.findColour(ListBox::backgroundColourId);
                    const auto c = rowIsSelected ? defaultColour.interpolatedWith(textColour, 0.5f) : defaultColour;

                    g.fillAll(c);
                }

                g.setColour(textColour);
                g.drawText(
                    proc.getProgramName(rowNumber),
                    juce::Rectangle<int>{width, height}.reduced(2),
                    Justification::left,
                    true
                );
            }

            void selectedRowsChanged(int row) override
            {
                if (0 <= row)
                    proc.setCurrentProgram(row);
            }

        private:
            Component& owner;
            AudioProcessor& proc;
        };

        Model model{*this, *getAudioProcessor()};
        ListBox listBox{"Programs", &model};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProgramAudioProcessorEditor)
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginWindow)
};
