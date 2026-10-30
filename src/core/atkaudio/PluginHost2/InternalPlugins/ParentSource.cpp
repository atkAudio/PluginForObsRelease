#include "ParentSource.h"

#include <atkaudio/ObsAudioChannelSet.h>

namespace atk
{
ParentSourceProcessor::ParentSourceProcessor(Role nodeRole)
    : AudioPluginInstance(
          nodeRole == input ? BusesProperties().withOutput("Parent Source", getDefaultObsAudioChannelSet())
                            : BusesProperties().withInput("Parent Source", getDefaultObsAudioChannelSet())
      )
    , role(nodeRole)
{
}

void ParentSourceProcessor::setBridge(ParentSourceBridge* owner)
{
    generation.store(owner != nullptr ? owner->getGeneration() : 0, std::memory_order_relaxed);
    bridge.store(owner, std::memory_order_release);
}

ParentSourceProcessor::Role ParentSourceProcessor::getRole()
{
    return role;
}

bool ParentSourceProcessor::isParentDescription(juce::PluginDescription description)
{
    return description.pluginFormatName == "Internal"
        && (description.name == "Parent Source In" || description.name == "Parent Source Out");
}

void ParentSourceProcessor::fillInPluginDescription(juce::PluginDescription& description) const
{
    description.name = getName();
    description.descriptiveName = getName();
    description.pluginFormatName = "Internal";
    description.fileOrIdentifier = getName();
    description.category = "OBS";
    description.uniqueId = description.deprecatedUid = getName().hashCode();
    description.numInputChannels = getTotalNumInputChannels();
    description.numOutputChannels = getTotalNumOutputChannels();
}

const juce::String ParentSourceProcessor::getName() const
{
    return role == input ? "Parent Source In" : "Parent Source Out";
}

void ParentSourceProcessor::prepareToPlay(double sampleRate, int)
{
    processingSampleRate = sampleRate;
}

void ParentSourceProcessor::releaseResources()
{
}

void ParentSourceProcessor::processBlock(juce::AudioBuffer<float>& samples, juce::MidiBuffer& midi)
{
    processBlock(samples, midi, juce::Time::getMillisecondCounterHiRes() * 0.001);
}

void ParentSourceProcessor::processBlock(juce::AudioBuffer<float>& samples, juce::MidiBuffer& midi, double timeSeconds)
{
    midi.clear();
    auto* owner = bridge.load(std::memory_order_acquire);
    auto activeGeneration = generation.load(std::memory_order_relaxed);
    if (role == input)
        if (owner != nullptr)
            owner->readInput(samples, processingSampleRate, timeSeconds, activeGeneration);
        else
            samples.clear();
    else if (owner != nullptr)
        owner->writeOutput(samples, processingSampleRate, timeSeconds, activeGeneration);
}

bool ParentSourceProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    auto& active = role == input ? layout.outputBuses : layout.inputBuses;
    auto& inactive = role == input ? layout.inputBuses : layout.outputBuses;
    return inactive.isEmpty() && active.size() == 1 && !active[0].isDisabled();
}

double ParentSourceProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

bool ParentSourceProcessor::acceptsMidi() const
{
    return false;
}

bool ParentSourceProcessor::producesMidi() const
{
    return false;
}

juce::AudioProcessorEditor* ParentSourceProcessor::createEditor()
{
    return nullptr;
}

bool ParentSourceProcessor::hasEditor() const
{
    return false;
}

int ParentSourceProcessor::getNumPrograms()
{
    return 1;
}

int ParentSourceProcessor::getCurrentProgram()
{
    return 0;
}

void ParentSourceProcessor::setCurrentProgram(int)
{
}

const juce::String ParentSourceProcessor::getProgramName(int)
{
    return {};
}

void ParentSourceProcessor::changeProgramName(int, const juce::String&)
{
}

void ParentSourceProcessor::getStateInformation(juce::MemoryBlock&)
{
}

void ParentSourceProcessor::setStateInformation(const void*, int)
{
}
} // namespace atk
