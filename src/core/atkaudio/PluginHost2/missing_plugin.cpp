#include "missing_plugin.h"

MissingPlugin::BusesProperties
MissingPlugin::savedBuses(const juce::XmlElement& xml, const juce::PluginDescription& description)
{
    juce::AudioProcessor::BusesProperties properties;
    auto* layout = xml.getChildByName("LAYOUT");
    for (bool input : {true, false})
    {
        if (layout == nullptr)
        {
            int channels = input ? description.numInputChannels : description.numOutputChannels;
            if (channels > 0)
                properties.addBus(input, input ? "Input" : "Output", juce::AudioChannelSet::discreteChannels(channels));
            continue;
        }
        if (auto* buses = layout->getChildByName(input ? "INPUTS" : "OUTPUTS"))
            for (auto* bus : buses->getChildWithTagNameIterator("BUS"))
            {
                auto text = bus->getStringAttribute("layout");
                auto channels = text == "disabled" ? juce::AudioChannelSet::disabled()
                                                   : juce::AudioChannelSet::fromAbbreviatedString(text);
                properties.addBus(
                    input,
                    (input ? "Input " : "Output ") + juce::String(bus->getIntAttribute("index") + 1),
                    channels.isDisabled() ? juce::AudioChannelSet::mono() : channels,
                    !channels.isDisabled()
                );
            }
    }
    return properties;
}

namespace
{
template <typename Sample>
void passThrough(
    juce::AudioBuffer<Sample>& buffer,
    juce::AudioBuffer<Sample>& scratch,
    const std::vector<int>& sourceChannels
)
{
    int samples = buffer.getNumSamples();
    if (samples > scratch.getNumSamples() || scratch.getNumChannels() > buffer.getNumChannels())
    {
        jassertfalse;
        buffer.clear();
        return;
    }
    for (int channel = 0; channel < scratch.getNumChannels(); ++channel)
        scratch.copyFrom(channel, 0, buffer, channel, 0, samples);
    buffer.clear();
    for (int channel = 0; channel < static_cast<int>(sourceChannels.size()); ++channel)
        if (sourceChannels[channel] >= 0)
            buffer.copyFrom(channel, 0, scratch, sourceChannels[channel], 0, samples);
}
} // namespace

MissingPlugin::MissingPlugin(
    const juce::XmlElement& xml,
    const juce::PluginDescription& pluginDescription,
    juce::String reason,
    bool hasMidiInput,
    bool hasMidiOutput
)
    : AudioPluginInstance(savedBuses(xml, pluginDescription))
    , originalXml(xml)
    , description(pluginDescription)
    , failureReason(std::move(reason))
    , midiInput(hasMidiInput)
    , midiOutput(hasMidiOutput)
    , savedLayout(getBusesLayout())
{
    if (auto* saved = xml.getChildByName("STATE"))
        state.fromBase64Encoding(saved->getAllSubText());
}

void MissingPlugin::fillInPluginDescription(juce::PluginDescription& target) const
{
    target = description;
}

const juce::String MissingPlugin::getName() const
{
    return description.name;
}

void MissingPlugin::prepareToPlay(double, int blockSize)
{
    sourceChannels.assign(getTotalNumOutputChannels(), -1);
    for (int bus = 0; bus < juce::jmin(getBusCount(true), getBusCount(false)); ++bus)
    {
        int inputOffset = getChannelIndexInProcessBlockBuffer(true, bus, 0);
        int outputOffset = getChannelIndexInProcessBlockBuffer(false, bus, 0);
        for (int channel = 0; channel < juce::jmin(getChannelCountOfBus(true, bus), getChannelCountOfBus(false, bus));
             ++channel)
            sourceChannels[outputOffset + channel] = inputOffset + channel;
    }
    floatScratch.setSize(getTotalNumInputChannels(), blockSize);
    doubleScratch.setSize(getTotalNumInputChannels(), blockSize);
}

void MissingPlugin::releaseResources()
{
}

void MissingPlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    passThrough(buffer, floatScratch, sourceChannels);
    if (!midiInput || !midiOutput)
        midi.clear();
}

void MissingPlugin::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi)
{
    passThrough(buffer, doubleScratch, sourceChannels);
    if (!midiInput || !midiOutput)
        midi.clear();
}

void MissingPlugin::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    processBlock(buffer, midi);
}

void MissingPlugin::processBlockBypassed(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi)
{
    processBlock(buffer, midi);
}

bool MissingPlugin::supportsDoublePrecisionProcessing() const
{
    return true;
}

bool MissingPlugin::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout == savedLayout;
}

double MissingPlugin::getTailLengthSeconds() const
{
    return 0.0;
}

bool MissingPlugin::acceptsMidi() const
{
    return midiInput;
}

bool MissingPlugin::producesMidi() const
{
    return midiOutput;
}

juce::AudioProcessorEditor* MissingPlugin::createEditor()
{
    return nullptr;
}

bool MissingPlugin::hasEditor() const
{
    return false;
}

int MissingPlugin::getNumPrograms()
{
    return 1;
}

int MissingPlugin::getCurrentProgram()
{
    return 0;
}

void MissingPlugin::setCurrentProgram(int)
{
}

const juce::String MissingPlugin::getProgramName(int)
{
    return {};
}

void MissingPlugin::changeProgramName(int, const juce::String&)
{
}

void MissingPlugin::getStateInformation(juce::MemoryBlock& target)
{
    target = state;
}

void MissingPlugin::setStateInformation(const void* data, int size)
{
    juce::MemoryBlock incoming(data, static_cast<size_t>(size));
    if (state == incoming)
        return;
    state = incoming;
    auto* saved = originalXml.getChildByName("STATE");
    if (saved == nullptr)
        saved = originalXml.createNewChildElement("STATE");
    saved->deleteAllChildElements();
    saved->addTextElement(state.toBase64Encoding());
}
