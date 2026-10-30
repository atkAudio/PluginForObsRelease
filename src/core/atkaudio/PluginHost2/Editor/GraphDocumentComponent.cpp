#include "GraphEditorPanel.h"
#include "MainHostWindow.h"
#include "../../Logging.h"

struct GraphDocumentComponent::TooltipBar
    : public Component
    , private Timer
{
    TooltipBar()
    {
        startTimer(100);
    }

    void paint(Graphics& graphics) override
    {
        graphics.setFont(FontOptions(getHeight() * 0.7f, Font::bold));
        graphics.setColour(Colours::black);
        graphics.drawFittedText(tip, 10, 0, jmax(0, getWidth() - 12), getHeight(), Justification::centredLeft, 1);
    }

    void timerCallback() override
    {
        String nextTip;
        if (auto* underMouse = Desktop::getInstance().getMainMouseSource().getComponentUnderMouse())
            if (auto* client = dynamic_cast<TooltipClient*>(underMouse))
                if (!underMouse->isMouseButtonDown() && !underMouse->isCurrentlyBlockedByAnotherModalComponent())
                    nextTip = client->getTooltip();
        if (tip != nextTip)
        {
            tip = nextTip;
            repaint();
        }
    }

    String tip;
};

GraphDocumentComponent::GraphDocumentComponent(
    MainHostWindow& mainWindow,
    PluginGraph& graphModel,
    AudioDeviceManager& dm,
    KnownPluginList& kpl
)
    : graph(&graphModel)
    , deviceManager(dm)
    , pluginList(kpl)
    , mainHostWindow(mainWindow)
{
    atk::logging::debug("GraphDocumentComponent::ctor", "begin");

    init();

    deviceManager.addChangeListener(graphPanel.get());
    deviceManager.addChangeListener(this);

    startTimer(100);

    atk::logging::debug("GraphDocumentComponent::ctor", "completed");
}

void GraphDocumentComponent::setCpuLoad()
{
    auto cpuLoad = mainHostWindow.getRuntimeCpuLoad();

    auto latencySamples = graph != nullptr ? graph->graph.getLatencySamples() : 0;
    auto* currentDevice = deviceManager.getCurrentAudioDevice();
    auto latencyMs = latencySamples > 0 && currentDevice != nullptr
                       ? (int)std::round((float)latencySamples / currentDevice->getCurrentSampleRate() * 1000.0f)
                       : 0;

    cpuLoadLabel.setText(
        "dly: " + juce::String(latencyMs) + "ms, " + "cpu: " + juce::String(cpuLoad, 2).replace("0.", "."),
        juce::dontSendNotification
    );
}

void GraphDocumentComponent::init()
{
    atk::logging::debug("GraphDocumentComponent::init", "begin");

    graphPanel.reset(new GraphEditorPanel(*graph));
    graphViewport = std::make_unique<GraphViewport>(*graphPanel);
    addAndMakeVisible(graphViewport.get());

    // Connect virtual keyboard to inject MIDI into MidiServer
    keyState.addListener(this);

    keyboardComp.reset(new MidiKeyboardComponent(keyState, MidiKeyboardComponent::horizontalKeyboard));
    addAndMakeVisible(keyboardComp.get());

    statusBar = std::make_unique<TooltipBar>();
    addAndMakeVisible(statusBar.get());

    addAndMakeVisible(cpuLoadLabel);
    cpuLoadLabel.setText("CPU Load: --", juce::dontSendNotification);
    cpuLoadLabel.setJustificationType(juce::Justification::centredRight);

    graphPanel->updateComponents();

    atk::logging::debug("GraphDocumentComponent::init", "completed");
}

GraphDocumentComponent::~GraphDocumentComponent()
{
    atk::logging::debug("GraphDocumentComponent::dtor", "begin");

    keyState.removeListener(this);
    releaseGraph();

    atk::logging::debug("GraphDocumentComponent::dtor", "completed");
}

void GraphDocumentComponent::resized()
{
    auto area = getLocalBounds();
    cpuLoadLabel.setBounds(area.removeFromTop(24).removeFromRight(180));

    auto r = [this]
    {
        auto bounds = getLocalBounds();

        if (auto* display = Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds()))
            return display->safeAreaInsets.subtractedFrom(bounds);

        return bounds;
    }();

    const int keysHeight = 60;
    const int statusHeight = 20;

    r.removeFromTop(24);
    keyboardComp->setBounds(r.removeFromBottom(keysHeight));
    statusBar->setBounds(r.removeFromBottom(statusHeight));
    graphViewport->setBounds(r);
}

void GraphDocumentComponent::createNewPlugin(const PluginDescriptionAndPreference& desc, Point<int> pos)
{
    if (graphPanel != nullptr)
        graphPanel->createNewPlugin(desc, graphPanel->getLocalPoint(this, pos));
}

void GraphDocumentComponent::releaseGraph()
{
    deviceManager.removeChangeListener(this);

    if (graphPanel != nullptr)
    {
        deviceManager.removeChangeListener(graphPanel.get());
        graphViewport = nullptr;
        graphPanel = nullptr;
    }

    keyboardComp = nullptr;
    statusBar = nullptr;

    graph = nullptr;
}

bool GraphDocumentComponent::isInterestedInDragSource(const SourceDetails& details)
{
    return (
        (dynamic_cast<ListBox*>(details.sourceComponent.get()) != nullptr)
        && details.description.toString().startsWith("PLUGIN")
    );
}

void GraphDocumentComponent::itemDropped(const SourceDetails& details)
{
    auto pluginTypeIndex = details.description.toString().fromFirstOccurrenceOf("PLUGIN: ", false, false).getIntValue();

    // must be a valid index!
    jassert(isPositiveAndBelow(pluginTypeIndex, pluginList.getNumTypes()));
    if (!isPositiveAndBelow(pluginTypeIndex, pluginList.getNumTypes()))
        return;

    createNewPlugin(PluginDescriptionAndPreference{pluginList.getTypes()[pluginTypeIndex]}, details.localPosition);
}

void GraphDocumentComponent::changeListenerCallback(ChangeBroadcaster*)
{
    if (graphPanel != nullptr)
    {
        graphPanel->updateComponents();
        graphPanel->repaint();
    }

    // Remove any illegal connections that might have been created due to channel count changes
    if (graph != nullptr)
        graph->graph.removeIllegalConnections();
}

void GraphDocumentComponent::handleNoteOn(
    juce::MidiKeyboardState* source,
    int midiChannel,
    int midiNoteNumber,
    float velocity
)
{
    juce::ignoreUnused(source);

    // Inject virtual keyboard MIDI into MidiServer
    auto message = juce::MidiMessage::noteOn(midiChannel, midiNoteNumber, velocity);
    juce::MidiBuffer buffer;
    buffer.addEvent(message, 0);
    mainHostWindow.getMidiClient().injectMidi(buffer);
}

void GraphDocumentComponent::handleNoteOff(
    juce::MidiKeyboardState* source,
    int midiChannel,
    int midiNoteNumber,
    float velocity
)
{
    juce::ignoreUnused(source);

    // Inject virtual keyboard MIDI into MidiServer
    auto message = juce::MidiMessage::noteOff(midiChannel, midiNoteNumber, velocity);
    juce::MidiBuffer buffer;
    buffer.addEvent(message, 0);
    mainHostWindow.getMidiClient().injectMidi(buffer);
}
