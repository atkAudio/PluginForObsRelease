#include "midi_obs_mapping.h"

namespace
{
juce::var serializeMapping(const atk::MidiObsMapping& mapping)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("mappingId", mapping.mappingId);
    object->setProperty("inputDeviceName", mapping.inputDeviceName);
    object->setProperty("messageType", mapping.messageType);
    object->setProperty("interactionMode", mapping.interactionMode);
    object->setProperty("channel", mapping.channel);
    object->setProperty("data1", mapping.data1);
    object->setProperty("actionType", mapping.actionType);
    object->setProperty("targetUuid", mapping.targetUuid);
    object->setProperty("targetName", mapping.targetName);
    object->setProperty("enabled", mapping.enabled);
    object->setProperty("valueInverted", mapping.valueInverted);
    object->setProperty("minValue", mapping.minValue);
    object->setProperty("maxValue", mapping.maxValue);
    return juce::var(object);
}

atk::MidiObsMapping deserializeMapping(const juce::var& value)
{
    atk::MidiObsMapping mapping;

    if (auto* object = value.getDynamicObject())
    {
        mapping.mappingId = object->getProperty("mappingId").toString();
        mapping.inputDeviceName = object->getProperty("inputDeviceName").toString();
        mapping.messageType = int(object->getProperty("messageType"));
        mapping.interactionMode = int(object->getProperty("interactionMode"));
        mapping.channel = int(object->getProperty("channel"));
        mapping.data1 = int(object->getProperty("data1"));
        mapping.actionType = int(object->getProperty("actionType"));
        mapping.targetUuid = object->getProperty("targetUuid").toString();
        mapping.targetName = object->getProperty("targetName").toString();
        mapping.enabled = bool(object->getProperty("enabled"));
        mapping.valueInverted = bool(object->getProperty("valueInverted"));
        mapping.minValue = double(object->getProperty("minValue"));
        mapping.maxValue = double(object->getProperty("maxValue"));
    }

    if (mapping.mappingId.isEmpty())
        mapping.mappingId = atk::createMidiObsMappingId();

    if (mapping.channel < 0)
        mapping.channel = 0;

    if (mapping.channel > 16)
        mapping.channel = 16;

    if (mapping.data1 < 0)
        mapping.data1 = 0;

    if (mapping.maxValue < mapping.minValue)
        std::swap(mapping.minValue, mapping.maxValue);

    return mapping;
}
} // namespace

namespace atk
{

juce::String createMidiObsMappingId()
{
    return juce::Uuid().toString();
}

juce::String serializeMidiObsMappings(const std::vector<MidiObsMapping>& mappings)
{
    juce::Array<juce::var> items;

    for (auto& mapping : mappings)
        items.add(serializeMapping(mapping));

    return juce::JSON::toString(juce::var(items));
}

bool deserializeMidiObsMappings(const juce::String& data, std::vector<MidiObsMapping>& mappings)
{
    mappings.clear();

    if (data.trim().isEmpty())
        return true;

    auto parsed = juce::JSON::parse(data);
    if (!parsed.isArray())
        return false;

    auto* array = parsed.getArray();
    if (array == nullptr)
        return false;

    mappings.reserve(array->size());

    for (auto& item : *array)
        mappings.push_back(deserializeMapping(item));

    return true;
}

} // namespace atk
