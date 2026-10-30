#include "Transport.h"
#include "../PluginGraph.h"
#include "../Editor/TransportEditor.h"

TransportProcessor::TransportProcessor()
    : AudioPluginInstance(BusesProperties{})
{
}

TransportProcessor::~TransportProcessor()
{
    bind(nullptr);
}

bool TransportProcessor::isTransportDescription(juce::PluginDescription description)
{
    return description.pluginFormatName == "Internal" && description.name == "Transport";
}

void TransportProcessor::bind(PluginGraph* owner, bool enabled)
{
    auto* previous = shared.load(std::memory_order_acquire);
    auto identifier = generation.load(std::memory_order_acquire);
    if (graph == owner && ((identifier != 0) == enabled || owner == nullptr))
        return;
    if (previous != nullptr)
    {
        savedSettings = previous->get();
        savedPosition = previous->quarterNotes.load(std::memory_order_relaxed);
        previous->deactivate(identifier);
    }
    generation.store(0, std::memory_order_release);
    graph = owner;
    auto* next = owner == nullptr ? nullptr : &owner->getPublishedTransport();
    shared.store(next, std::memory_order_release);
    if (next != nullptr && enabled)
        generation.store(next->activate(savedSettings, savedModes, savedPosition), std::memory_order_release);
}

bool TransportProcessor::command(TransportOperation operation, double value)
{
    auto* owner = shared.load(std::memory_order_acquire);
    if (owner == nullptr)
        return false;
    return owner->request({generation.load(std::memory_order_acquire), operation, value});
}

TransportSettings TransportProcessor::getSettings()
{
    if (auto* owner = shared.load(std::memory_order_acquire))
        return owner->get();
    return savedSettings;
}

bool TransportProcessor::setSettings(TransportSettings settings, bool continueTapSequence)
{
    if (!settings.isValid())
        return false;
    if (graph != nullptr && graph->setTransportSettings(settings, continueTapSequence).failed())
        return false;
    savedSettings = settings;
    updateHostDisplay();
    return true;
}

int TransportProcessor::getModes()
{
    return savedModes;
}

void TransportProcessor::setModes(int modes)
{
    if (modes < 0 || modes > 127 || modes == savedModes)
        return;
    savedModes = modes;
    if (auto* owner = shared.load(std::memory_order_acquire))
        owner->modes.store(modes, std::memory_order_release);
    updateHostDisplay();
}

double TransportProcessor::getQuarterNotes()
{
    if (auto* owner = shared.load(std::memory_order_acquire))
        return owner->quarterNotes.load(std::memory_order_relaxed);
    return savedPosition;
}

bool TransportProcessor::isPlaying()
{
    if (auto* owner = shared.load(std::memory_order_acquire))
        return owner->playing.load(std::memory_order_relaxed);
    return false;
}

bool TransportProcessor::isPaused()
{
    if (auto* owner = shared.load(std::memory_order_acquire))
        return owner->paused.load(std::memory_order_relaxed);
    return false;
}

juce::String TransportProcessor::getPositionText()
{
    auto settings = getSettings();
    int ticksPerBeat = 3840 / settings.denominator;
    int64_t ticksPerBar = ticksPerBeat * settings.numerator;
    int64_t ticks = std::llround(getQuarterNotes() * 960.0);
    return juce::String(ticks / ticksPerBar + 1).paddedLeft('0', 3)
         + ":"
         + juce::String((ticks % ticksPerBar) / ticksPerBeat + 1).paddedLeft('0', 2)
         + ":"
         + juce::String(ticks % ticksPerBeat).paddedLeft('0', 3);
}

bool TransportProcessor::seekPositionText(juce::String text)
{
    auto fields = juce::StringArray::fromTokens(text, ":", "");
    if (fields.size() != 3)
        return false;
    for (auto& field : fields)
        if (field.isEmpty() || !field.containsOnly("0123456789") || field.length() > 11)
            return false;
    int64_t bar = fields[0].getLargeIntValue();
    int64_t beat = fields[1].getLargeIntValue();
    int64_t tick = fields[2].getLargeIntValue();
    auto settings = getSettings();
    if (bar < 1 || beat < 1 || beat > settings.numerator || tick < 0 || tick >= 3840 / settings.denominator)
        return false;
    double destination = ((bar - 1) * settings.numerator + (beat - 1)) * 4.0 / settings.denominator + tick / 960.0;
    return destination <= 1000000000.0 && command(transportSeek, destination);
}

int TransportProcessor::getEffectiveTempo()
{
    if (auto* owner = shared.load(std::memory_order_acquire))
        return owner->effectiveTempo.load(std::memory_order_relaxed);
    return savedSettings.tempoHundredths;
}

int TransportProcessor::getSyncStatus()
{
    if (auto* owner = shared.load(std::memory_order_acquire))
        return owner->syncStatus.load(std::memory_order_relaxed);
    return 0;
}

uint32_t TransportProcessor::getRejectedCommands()
{
    if (auto* owner = shared.load(std::memory_order_acquire))
        return owner->rejectedCommands.load(std::memory_order_relaxed);
    return 0;
}

void TransportProcessor::prepareToPlay(double, int)
{
}

void TransportProcessor::releaseResources()
{
}

void TransportProcessor::processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer& midi)
{
    auto* owner = shared.load(std::memory_order_acquire);
    auto identifier = generation.load(std::memory_order_acquire);
    if (owner == nullptr || identifier == 0 || identifier != owner->activeGeneration.load(std::memory_order_acquire))
    {
        midi.clear();
        return;
    }
    int modes = owner->modes.load(std::memory_order_acquire);
    int64_t blockStart = owner->renderSample.load(std::memory_order_acquire);
    for (auto event : midi)
    {
        auto message = event.getMessage();
        TransportCommand request{identifier};
        bool recognized = true;
        if (message.isMidiClock() && (modes & followClock) != 0)
        {
            request.operation = transportMidiClock;
            request.value = static_cast<double>(blockStart + event.samplePosition);
        }
        else if (message.isSongPositionPointer() && (modes & followPosition) != 0)
        {
            request.operation = transportSeek;
            request.value = message.getSongPositionPointerMidiBeat() / 4.0;
        }
        else if (message.isMidiStart() && (modes & followPlayback) != 0)
            request.operation = transportMidiStart;
        else if (message.isMidiContinue() && (modes & followPlayback) != 0)
            request.operation = transportMidiContinue;
        else if (message.isMidiStop() && (modes & followPlayback) != 0)
            request.operation = transportMidiStop;
        else
            recognized = false;
        if (recognized)
            owner->request(request, true);
    }
    midi.clear();
    midi.addEvents(owner->generatedMidi, 0, -1, 0);
}

void TransportProcessor::processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer& midi)
{
    midi.clear();
}

void TransportProcessor::fillInPluginDescription(juce::PluginDescription& description) const
{
    description.name = description.descriptiveName = description.fileOrIdentifier = "Transport";
    description.pluginFormatName = "Internal";
    description.category = "Utility";
    description.uniqueId = description.deprecatedUid = description.name.hashCode();
    description.isInstrument = false;
}

const juce::String TransportProcessor::getName() const
{
    return "Transport";
}

bool TransportProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout.inputBuses.isEmpty() && layout.outputBuses.isEmpty();
}

double TransportProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

bool TransportProcessor::acceptsMidi() const
{
    return true;
}

bool TransportProcessor::producesMidi() const
{
    return true;
}

bool TransportProcessor::isMidiEffect() const
{
    return true;
}

juce::AudioProcessorEditor* TransportProcessor::createEditor()
{
    return new TransportEditor(*this);
}

bool TransportProcessor::hasEditor() const
{
    return true;
}

int TransportProcessor::getNumPrograms()
{
    return 1;
}

int TransportProcessor::getCurrentProgram()
{
    return 0;
}

void TransportProcessor::setCurrentProgram(int)
{
}

const juce::String TransportProcessor::getProgramName(int)
{
    return {};
}

void TransportProcessor::changeProgramName(int, const juce::String&)
{
}

void TransportProcessor::getStateInformation(juce::MemoryBlock& block)
{
    auto settings = getSettings();
    juce::XmlElement xml("TRANSPORT");
    xml.setAttribute("tempoHundredths", settings.tempoHundredths);
    xml.setAttribute("numerator", settings.numerator);
    xml.setAttribute("denominator", settings.denominator);
    xml.setAttribute("modes", savedModes);
    xml.setAttribute("position", getQuarterNotes());
    copyXmlToBinary(xml, block);
}

bool TransportProcessor::readState(const void* data, int size, TransportSettings& settings, int& modes, double& position)
{
    auto xml = getXmlFromBinary(data, size);
    if (xml == nullptr || !xml->hasTagName("TRANSPORT"))
        return false;
    auto integer = [&](const char* name, int& value)
    {
        auto text = xml->getStringAttribute(name);
        value = text.getIntValue();
        return text == juce::String(value);
    };
    juce::String positionText = xml->getStringAttribute("position", "0");
    if (!positionText.containsOnly("0123456789.eE+-"))
        return false;
    auto end = positionText.getCharPointer();
    position = juce::CharacterFunctions::readDoubleValue(end);
    return integer("tempoHundredths", settings.tempoHundredths)
        && integer("numerator", settings.numerator)
        && integer("denominator", settings.denominator)
        && integer("modes", modes)
        && settings.isValid()
        && modes >= 0
        && modes <= 127
        && positionText.isNotEmpty()
        && end.isEmpty()
        && position >= 0.0
        && position <= 1000000000.0;
}

void TransportProcessor::setStateInformation(const void* data, int size)
{
    TransportSettings settings;
    int modes = 0;
    double position = 0.0;
    if (!readState(data, size, settings, modes, position))
        return;
    savedSettings = settings;
    savedModes = modes;
    savedPosition = position;
    if (auto* owner = shared.load(std::memory_order_acquire))
    {
        owner->deactivate(generation.load(std::memory_order_acquire));
        generation.store(owner->activate(settings, modes, position), std::memory_order_release);
    }
    updateHostDisplay();
}
