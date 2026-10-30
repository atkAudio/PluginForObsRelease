#include "ChordAnalyzerPlugin.h"

namespace
{
class ChordAnalyzerEditor
    : public juce::AudioProcessorEditor
    , private juce::Timer
{
public:
    ChordAnalyzerEditor(ChordAnalyzerPlugin& processor)
        : AudioProcessorEditor(processor)
        , owner(processor)
    {
        chord.setJustificationType(juce::Justification::centred);
        chord.setFont(juce::FontOptions(36.0f));
        notes.setJustificationType(juce::Justification::centred);
        send.setButtonText("Send Chord Context to Plugins");
        send.setTooltip("Provide the detected chord as VST3 chord context to all plugins in this host.");
        send.onClick = [this]
        {
            owner.setSendChordContext(send.getToggleState());
        };
        addAndMakeVisible(chord);
        addAndMakeVisible(notes);
        addAndMakeVisible(send);
        setSize(420, 180);
        timerCallback();
        startTimerHz(30);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16);
        chord.setBounds(area.removeFromTop(64));
        notes.setBounds(area.removeFromTop(36));
        send.setBounds(area.removeFromBottom(28));
    }

private:
    ChordAnalyzerPlugin& owner;
    juce::Label chord;
    juce::Label notes;
    juce::ToggleButton send;

    void timerCallback() override
    {
        auto result = owner.getResult();
        juce::String title = "No Chord";
        if (result.status == chord_analyzer::settling)
            title = "...";
        else if (result.status == chord_analyzer::unknown)
            title = "Unknown";
        else if (result.status == chord_analyzer::recognized)
        {
            title = juce::String(chord_analyzer::NoteName(result.root)) + chord_analyzer::Suffix(result.type);
            if (result.bass % 12 != result.root)
                title += "/" + juce::String(chord_analyzer::NoteName(result.bass));
        }
        juce::StringArray names;
        for (int pitch = 0; pitch < 12; ++pitch)
            if ((result.pitches & (1 << pitch)) != 0)
                names.add(chord_analyzer::NoteName(pitch));
        chord.setText(title, juce::dontSendNotification);
        notes.setText(names.joinIntoString("  "), juce::dontSendNotification);
        send.setToggleState(owner.getSendChordContext(), juce::dontSendNotification);
    }
};
} // namespace

ChordAnalyzerPlugin::ChordAnalyzerPlugin()
    : AudioPluginInstance(BusesProperties{})
{
}

ChordAnalyzerPlugin::~ChordAnalyzerPlugin()
{
    bind(nullptr, false);
}

bool ChordAnalyzerPlugin::isDescription(juce::PluginDescription description)
{
    return description.pluginFormatName == "Internal" && description.name == "Chord Analyzer";
}

void ChordAnalyzerPlugin::bind(std::shared_ptr<chord_analyzer::PublishedChord> next, bool enabled)
{
    auto* previous = context.load(std::memory_order_acquire);
    uint32_t identifier = generation.load(std::memory_order_acquire);
    if (next.get() == previous && (identifier != 0) == enabled)
        return;
    if (previous != nullptr)
        previous->deactivate(identifier);
    generation.store(0, std::memory_order_release);
    if (next == nullptr)
        return;
    jassert(previous == nullptr || previous == next.get());
    if (previous == nullptr)
    {
        contextLifetime = next;
        context.store(next.get(), std::memory_order_release);
    }
    if (enabled)
        generation.store(next->activate(getSendChordContext()), std::memory_order_release);
}

chord_analyzer::Result ChordAnalyzerPlugin::getResult()
{
    if (context.load(std::memory_order_acquire) != nullptr && generation.load(std::memory_order_acquire) == 0)
        return {};
    return chord_analyzer::Result::decode(display.load(std::memory_order_acquire));
}

bool ChordAnalyzerPlugin::getSendChordContext()
{
    return sendChordContext.load(std::memory_order_acquire);
}

void ChordAnalyzerPlugin::setSendChordContext(bool enabled)
{
    sendChordContext.store(enabled, std::memory_order_release);
    if (auto* owner = context.load(std::memory_order_acquire))
        if (generation.load(std::memory_order_acquire) != 0)
            owner->enabled.store(enabled, std::memory_order_release);
    updateHostDisplay();
}

void ChordAnalyzerPlugin::prepareToPlay(double sampleRate, int)
{
    analyzer.prepare(sampleRate);
    processingGeneration = generation.load(std::memory_order_acquire);
    display.store(0, std::memory_order_release);
    if (auto* owner = context.load(std::memory_order_acquire))
        owner->publish(processingGeneration, {});
}

void ChordAnalyzerPlugin::releaseResources()
{
    analyzer.reset();
    display.store(0, std::memory_order_release);
    if (auto* owner = context.load(std::memory_order_acquire))
        owner->publish(generation.load(std::memory_order_acquire), {});
}

void ChordAnalyzerPlugin::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    uint32_t identifier = generation.load(std::memory_order_acquire);
    auto* owner = context.load(std::memory_order_acquire);
    if (identifier != processingGeneration)
    {
        analyzer.reset();
        processingGeneration = identifier;
    }
    if (owner != nullptr && (identifier == 0 || identifier != owner->generation.load(std::memory_order_acquire)))
    {
        display.store(0, std::memory_order_release);
        return;
    }
    int offset = 0;
    for (auto event : midi)
    {
        int position = juce::jlimit(offset, audio.getNumSamples(), event.samplePosition);
        analyzer.advance(position - offset);
        offset = position;
        if (event.numBytes >= 3 && event.data[0] >= 0x80 && event.data[0] < 0xf0)
            analyzer.message(event.data[0] & 15, event.data[0] & 0xf0, event.data[1], event.data[2]);
    }
    analyzer.advance(audio.getNumSamples() - offset);
    auto result = analyzer.getResult();
    display.store(result.encode(), std::memory_order_release);
    if (owner != nullptr)
        owner->publish(identifier, result);
}

void ChordAnalyzerPlugin::processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    analyzer.reset();
    display.store(0, std::memory_order_release);
    if (auto* owner = context.load(std::memory_order_acquire))
        owner->publish(generation.load(std::memory_order_acquire), {});
}

void ChordAnalyzerPlugin::fillInPluginDescription(juce::PluginDescription& description) const
{
    description.name = description.descriptiveName = description.fileOrIdentifier = "Chord Analyzer";
    description.pluginFormatName = "Internal";
    description.category = "MIDI";
    description.manufacturerName = "atkAudio";
    description.version = "1.0";
    description.uniqueId = description.deprecatedUid = description.name.hashCode();
    description.numInputChannels = description.numOutputChannels = 0;
    description.isInstrument = false;
}

const juce::String ChordAnalyzerPlugin::getName() const
{
    return "Chord Analyzer";
}

bool ChordAnalyzerPlugin::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout.inputBuses.isEmpty() && layout.outputBuses.isEmpty();
}

double ChordAnalyzerPlugin::getTailLengthSeconds() const
{
    return 0;
}

bool ChordAnalyzerPlugin::acceptsMidi() const
{
    return true;
}

bool ChordAnalyzerPlugin::producesMidi() const
{
    return true;
}

bool ChordAnalyzerPlugin::isMidiEffect() const
{
    return true;
}

juce::AudioProcessorEditor* ChordAnalyzerPlugin::createEditor()
{
    return new ChordAnalyzerEditor(*this);
}

bool ChordAnalyzerPlugin::hasEditor() const
{
    return true;
}

int ChordAnalyzerPlugin::getNumPrograms()
{
    return 1;
}

int ChordAnalyzerPlugin::getCurrentProgram()
{
    return 0;
}

void ChordAnalyzerPlugin::setCurrentProgram(int)
{
}

const juce::String ChordAnalyzerPlugin::getProgramName(int)
{
    return {};
}

void ChordAnalyzerPlugin::changeProgramName(int, const juce::String&)
{
}

void ChordAnalyzerPlugin::getStateInformation(juce::MemoryBlock& state)
{
    juce::XmlElement xml("CHORD_ANALYZER");
    xml.setAttribute("sendChordContext", getSendChordContext());
    copyXmlToBinary(xml, state);
}

void ChordAnalyzerPlugin::setStateInformation(const void* data, int size)
{
    auto xml = getXmlFromBinary(data, size);
    if (xml != nullptr && xml->hasTagName("CHORD_ANALYZER"))
        setSendChordContext(xml->getBoolAttribute("sendChordContext", true));
}
