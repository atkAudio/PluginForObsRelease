#include "InternalPlugin.h"
#include "InternalPlugins.h"

#include <config.h>

juce::PluginDescription InternalPlugin::getPluginDescription(const juce::AudioProcessor& proc)
{
    const auto ins = proc.getTotalNumInputChannels();
    const auto outs = proc.getTotalNumOutputChannels();
    const auto identifier = proc.getName();
    const auto registerAsGenerator = ins == 0;
    const auto acceptsMidi = proc.acceptsMidi();

    juce::PluginDescription descr;

    descr.name = identifier;
    descr.descriptiveName = identifier;
    descr.pluginFormatName = InternalPluginFormat::getIdentifier();

    // Special category for OBS nodes
    if (identifier == "OBS Source" || identifier == "OBS Output")
        descr.category = "OBS";
    else
        descr.category = (registerAsGenerator ? (acceptsMidi ? "Synth" : "Generator") : "Effect");

    descr.manufacturerName = PLUGIN_AUTHOR;
    descr.version = PLUGIN_VERSION;
    descr.fileOrIdentifier = identifier;
    descr.isInstrument = (acceptsMidi && registerAsGenerator);
    descr.numInputChannels = ins;
    descr.numOutputChannels = outs;

    descr.uniqueId = descr.deprecatedUid = identifier.hashCode();

    return descr;
}
