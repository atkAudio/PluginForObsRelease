#pragma once

#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>

#include <juce_core/juce_core.h>

#include <vector>

namespace atk
{

enum MidiObsMessageType
{
    midi_obs_message_type_cc = 0,
    midi_obs_message_type_note,
    midi_obs_message_type_program_change,
};

enum MidiObsInteractionMode
{
    midi_obs_interaction_mode_absolute = 0,
    midi_obs_interaction_mode_trigger,
    midi_obs_interaction_mode_toggle,
};

enum MidiObsActionType
{
    midi_obs_action_type_source_volume = 0,
    midi_obs_action_type_source_mute,
    midi_obs_action_type_source_enabled,
    midi_obs_action_type_media_play_pause,
    midi_obs_action_type_media_restart,
    midi_obs_action_type_media_stop,
    midi_obs_action_type_scene,
    midi_obs_action_type_transition,
};

struct MidiObsMapping
{
    juce::String mappingId;
    juce::String inputDeviceName;
    int messageType = midi_obs_message_type_cc;
    int interactionMode = midi_obs_interaction_mode_absolute;
    int channel = 0;
    int data1 = 1;
    int actionType = midi_obs_action_type_source_volume;
    juce::String targetUuid;
    juce::String targetName;
    bool enabled = true;
    bool valueInverted = false;
    double minValue = 0.0;
    double maxValue = 1.0;
};

juce::String createMidiObsMappingId();
juce::String serializeMidiObsMappings(const std::vector<MidiObsMapping>& mappings);
bool deserializeMidiObsMappings(const juce::String& data, std::vector<MidiObsMapping>& mappings);

} // namespace atk
