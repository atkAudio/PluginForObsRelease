#pragma once

#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace atk
{

enum MidiControlMessageType
{
    midi_control_message_type_cc = 0,
    midi_control_message_type_note,
    midi_control_message_type_program_change,
};

enum MidiControlInteractionMode
{
    midi_control_interaction_mode_absolute = 0,
    midi_control_interaction_mode_trigger,
    midi_control_interaction_mode_toggle,
    midi_control_interaction_mode_inc_dec,
    midi_control_interaction_mode_inc_dec2,
};

enum MidiControlActionType
{
    midi_control_action_type_none = 0,
    midi_control_action_type_source,
    midi_control_action_type_media,
    midi_control_action_type_scene,
    midi_control_action_type_transition,
    midi_control_action_type_hotkey,
    midi_control_action_type_mapping_context,
    midi_control_action_type_last_touched,
    midi_control_action_type_effect_parm,
};

enum MidiControlMediaActionType
{
    midi_control_media_action_type_none = 0,
    midi_control_media_action_type_play_pause,
    midi_control_media_action_type_restart,
    midi_control_media_action_type_stop,
};

enum MidiControlSourceActionType
{
    midi_control_source_action_type_none = 0,
    midi_control_source_action_type_volume,
    midi_control_source_action_type_mute,
    midi_control_source_action_type_monitoring,
};

enum MidiControlMappingContextTargetType
{
    midi_control_mapping_context_target_type_none = 0,
    midi_control_mapping_context_target_type_bank,
    midi_control_mapping_context_target_type_preset,
};

enum MidiControlHotkeyTargetType
{
    midi_control_hotkey_target_type_frontend = 0,
};

struct MidiControlHotkeyTarget
{
    int targetType = midi_control_hotkey_target_type_frontend;
    uint64_t hotkeyId = 0;
};

enum MidiControlMappingContextRange
{
    midi_control_mapping_context_any = 0,
    midi_control_mapping_context_min = 1,
    midi_control_mapping_context_max = 8,
};

enum MidiControlOutputMode
{
    midi_control_output_mode_none = 0,
    midi_control_output_mode_any,
    midi_control_output_mode_specific,
};

enum MidiControlActivationMode
{
    midi_control_activation_mode_scene = 0,
    midi_control_activation_mode_global,
    midi_control_activation_mode_preview,
};

struct MidiControlMapping
{
    juce::String mappingId;
    juce::String inputDeviceName;
    int outputMode = midi_control_output_mode_none;
    juce::String outputDeviceName;
    int messageType = midi_control_message_type_cc;
    int interactionMode = midi_control_interaction_mode_absolute;
    int channel = 0;
    int data1 = 1;
    int actionType = midi_control_action_type_none;
    juce::String targetUuid;
    juce::String targetName;
    juce::String targetLayerToken;
    juce::String targetLayerName;
    juce::String targetParameterToken;
    juce::String targetParameterName;
    int bank = midi_control_mapping_context_any;
    int preset = midi_control_mapping_context_any;
    int activationMode = midi_control_activation_mode_scene;
    bool enabled = true;
    bool valueInverted = false;
    double minValue = 0.0;
    double maxValue = 1.0;
    double scale = 1.0;
};

struct MidiControlActionOption
{
    int actionType = midi_control_action_type_none;
    juce::String displayName;
};

struct MidiControlTargetOption
{
    juce::String targetToken;
    juce::String displayName;
};

bool validateMidiControlMapping(const MidiControlMapping& mapping, juce::String& errorMessage);
bool validateMidiControlMappings(const std::vector<MidiControlMapping>& mappings, juce::String& errorMessage);

juce::String createLastTouchedTargetToken(int offset);
bool parseLastTouchedTargetOffset(const juce::String& token, int& offset);
juce::String getLastTouchedTargetDisplayName(int offset);
juce::String createLastTouchedHoldTargetToken();
bool isLastTouchedHoldTargetToken(const juce::String& token);
juce::String getLastTouchedHoldTargetDisplayName();

juce::String createObsFilterAudioLastTouchedTargetToken(int offset);
bool parseObsFilterAudioLastTouchedTargetOffset(const juce::String& token, int& offset);
juce::String getObsFilterAudioLastTouchedTargetDisplayName(int offset);

juce::String createObsFilterVideoLastTouchedTargetToken(int offset);
bool parseObsFilterVideoLastTouchedTargetOffset(const juce::String& token, int& offset);
juce::String getObsFilterVideoLastTouchedTargetDisplayName(int offset);

juce::String createMidiControlMappingContextTargetToken(int targetType);
bool parseMidiControlMappingContextTargetType(const juce::String& token, int& targetType);
juce::String getMidiControlMappingContextTargetDisplayName(int targetType);
std::vector<MidiControlTargetOption> getMidiControlMappingContextTargetOptions();
juce::String createMidiControlMappingContextParameterToken(int directValue);
juce::String createMidiControlMappingContextParameterNextToken();
juce::String createMidiControlMappingContextParameterPreviousToken();
bool parseMidiControlMappingContextParameterToken(const juce::String& token, int& directValue, int& relativeStep);
juce::String getMidiControlMappingContextParameterDisplayName(const juce::String& token);
std::vector<MidiControlTargetOption> getMidiControlMappingContextParameterOptions();
std::vector<MidiControlActionOption> getMidiControlActionOptions();
std::vector<MidiControlTargetOption> getMidiControlTargetOptionsForAction(int actionType);
juce::String createMidiControlMediaActionTypeLayerToken(int mediaActionType);
bool parseMidiControlMediaActionType(const juce::String& token, int& mediaActionType);
juce::String getMidiControlMediaActionTypeDisplayName(int mediaActionType);
std::vector<MidiControlTargetOption> getMidiControlMediaActionTypeOptions();
juce::String createMidiControlSourceActionTypeLayerToken(int sourceActionType);
bool parseMidiControlSourceActionType(const juce::String& token, int& sourceActionType);
juce::String getMidiControlSourceActionTypeDisplayName(int sourceActionType);
std::vector<MidiControlTargetOption> getMidiControlSourceActionTypeOptions();

juce::String createMidiControlHotkeyIdTargetToken(uint64_t hotkeyId);
bool parseMidiControlHotkeyTargetToken(const juce::String& token, MidiControlHotkeyTarget& target);

juce::String createMidiControlMappingId();
juce::String serializeMidiControlMappings(const std::vector<MidiControlMapping>& mappings);
bool deserializeMidiControlMappings(const juce::String& data, std::vector<MidiControlMapping>& mappings);
juce::String serializeMidiControlMappingsFilePayload(const std::vector<MidiControlMapping>& mappings);
bool deserializeMidiControlMappingsFilePayload(
    const juce::String& data,
    std::vector<MidiControlMapping>& mappings,
    juce::String& errorMessage
);

} // namespace atk
