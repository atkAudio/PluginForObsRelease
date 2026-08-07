#include "midi_control_mapping.h"

#include <string_view>
#include <cmath>

namespace
{
constexpr int kMaxLastTouchedOffset = 7;
constexpr std::string_view kLastTouchedTargetPrefix = "plugin_parm:";
constexpr std::string_view kObsFilterAudioLastTouchedTargetPrefix = "obs_filter_audio_parm:";
constexpr std::string_view kObsFilterVideoLastTouchedTargetPrefix = "obs_filter_video_parm:";
constexpr std::string_view kLastTouchedHoldTargetToken = "plugin_parm:hold";
constexpr std::string_view kMappingContextTargetPrefix = "mapping_context_target:";
constexpr std::string_view kMappingContextParameterPrefix = "mapping_context_parameter:";
constexpr std::string_view kMappingTargetNextToken = "next";
constexpr std::string_view kMappingTargetPreviousToken = "prev";
constexpr std::string_view kMappingContextTargetBankToken = "bank";
constexpr std::string_view kMappingContextTargetPresetToken = "preset";
constexpr std::string_view kMediaActionLayerPrefix = "media_action:";
constexpr std::string_view kMediaActionPlayPauseToken = "play_pause";
constexpr std::string_view kMediaActionRestartToken = "restart";
constexpr std::string_view kMediaActionStopToken = "stop";
constexpr std::string_view kSourceActionLayerPrefix = "source_action:";
constexpr std::string_view kSourceActionVolumeToken = "volume";
constexpr std::string_view kSourceActionMuteToken = "mute";
constexpr std::string_view kSourceActionMonitoringToken = "monitoring";
constexpr std::string_view kHotkeyTargetPrefix = "obs_hotkey:";
constexpr std::string_view kHotkeyIdTargetPrefix = "obs_hotkey:id:";
constexpr std::string_view kMappingsFilePayloadFormat = "atk_midi_control_mappings";
constexpr int kMappingsFilePayloadVersion = 3;

juce::var serializeMapping(const atk::MidiControlMapping& mapping)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("mappingId", mapping.mappingId);
    object->setProperty("inputDeviceName", mapping.inputDeviceName);
    object->setProperty("outputMode", mapping.outputMode);
    object->setProperty("outputDeviceName", mapping.outputDeviceName);
    object->setProperty("messageType", mapping.messageType);
    object->setProperty("interactionMode", mapping.interactionMode);
    object->setProperty("channel", mapping.channel);
    object->setProperty("data1", mapping.data1);
    object->setProperty("actionType", mapping.actionType);
    object->setProperty("targetUuid", mapping.targetUuid);
    object->setProperty("targetName", mapping.targetName);
    object->setProperty("targetLayerToken", mapping.targetLayerToken);
    object->setProperty("targetLayerName", mapping.targetLayerName);
    object->setProperty("targetParameterToken", mapping.targetParameterToken);
    object->setProperty("targetParameterName", mapping.targetParameterName);
    object->setProperty("bank", mapping.bank);
    object->setProperty("preset", mapping.preset);
    object->setProperty("activationMode", mapping.activationMode);
    object->setProperty("enabled", mapping.enabled);
    object->setProperty("valueInverted", mapping.valueInverted);
    object->setProperty("minValue", mapping.minValue);
    object->setProperty("maxValue", mapping.maxValue);
    object->setProperty("scale", mapping.scale);
    return juce::var(object);
}

atk::MidiControlMapping deserializeMapping(const juce::var& value)
{
    atk::MidiControlMapping mapping;

    if (auto* object = value.getDynamicObject())
    {
        mapping.mappingId = object->getProperty("mappingId").toString();
        mapping.inputDeviceName = object->getProperty("inputDeviceName").toString();
        mapping.outputMode = int(object->getProperty("outputMode"));
        mapping.outputDeviceName = object->getProperty("outputDeviceName").toString();
        mapping.messageType = int(object->getProperty("messageType"));
        mapping.interactionMode = int(object->getProperty("interactionMode"));
        mapping.channel = int(object->getProperty("channel"));
        mapping.data1 = int(object->getProperty("data1"));
        mapping.actionType = int(object->getProperty("actionType"));
        mapping.targetUuid = object->getProperty("targetUuid").toString();
        mapping.targetName = object->getProperty("targetName").toString();
        mapping.targetLayerToken = object->getProperty("targetLayerToken").toString();
        mapping.targetLayerName = object->getProperty("targetLayerName").toString();
        mapping.targetParameterToken = object->getProperty("targetParameterToken").toString();
        mapping.targetParameterName = object->getProperty("targetParameterName").toString();

        if (object->hasProperty("bank"))
            mapping.bank = int(object->getProperty("bank"));

        if (object->hasProperty("preset"))
            mapping.preset = int(object->getProperty("preset"));

        mapping.activationMode = int(object->getProperty("activationMode"));
        mapping.enabled = bool(object->getProperty("enabled"));
        mapping.valueInverted = bool(object->getProperty("valueInverted"));
        mapping.minValue = double(object->getProperty("minValue"));
        mapping.maxValue = double(object->getProperty("maxValue"));

        if (object->hasProperty("scale"))
            mapping.scale = double(object->getProperty("scale"));
        else
            mapping.scale = -1.0;
    }

    return mapping;
}

bool isValidLastTouchedOffset(int offset)
{
    return offset >= 0 && offset <= kMaxLastTouchedOffset;
}

bool isValidMappingContextValue(int value)
{
    return value >= atk::midi_control_mapping_context_any && value <= atk::midi_control_mapping_context_max;
}

bool isValidMappingContextDirectTargetValue(int value)
{
    return value >= atk::midi_control_mapping_context_min && value <= atk::midi_control_mapping_context_max;
}

bool parseMappingContextDirectOrRelativeValueToken(
    const juce::String& token,
    std::string_view prefix,
    int& directValue,
    int& relativeStep
)
{
    directValue = 0;
    relativeStep = 0;

    if (!token.startsWithIgnoreCase(prefix.data()))
        return false;

    auto suffix = token.fromFirstOccurrenceOf(prefix.data(), false, false).trim();
    if (suffix.isEmpty())
        return false;

    if (suffix.equalsIgnoreCase(kMappingTargetNextToken.data()))
    {
        relativeStep = 1;
        return true;
    }

    if (suffix.equalsIgnoreCase(kMappingTargetPreviousToken.data()))
    {
        relativeStep = -1;
        return true;
    }

    if (!suffix.containsOnly("0123456789"))
        return false;

    auto value = suffix.getIntValue();
    if (!isValidMappingContextDirectTargetValue(value))
        return false;

    directValue = value;
    return true;
}

juce::String createMappingContextToken(std::string_view prefix, juce::String suffix)
{
    return juce::String(prefix.data()) + suffix;
}

juce::var createMappingsArrayVar(const std::vector<atk::MidiControlMapping>& mappings)
{
    juce::Array<juce::var> items;

    for (auto& mapping : mappings)
        items.add(serializeMapping(mapping));

    return juce::var(items);
}

} // namespace

namespace atk
{

juce::String createLastTouchedTargetToken(int offset)
{
    if (!isValidLastTouchedOffset(offset))
        return {};

    return juce::String(kLastTouchedTargetPrefix.data()) + juce::String(offset);
}

bool parseLastTouchedTargetOffset(const juce::String& token, int& offset)
{
    offset = -1;

    if (!token.startsWithIgnoreCase(kLastTouchedTargetPrefix.data()))
        return false;

    const auto suffix = token.fromFirstOccurrenceOf(kLastTouchedTargetPrefix.data(), false, false).trim();
    if (suffix.isEmpty() || !suffix.containsOnly("0123456789"))
        return false;

    const auto parsedOffset = suffix.getIntValue();
    if (!isValidLastTouchedOffset(parsedOffset))
        return false;

    offset = parsedOffset;
    return true;
}

juce::String getLastTouchedTargetDisplayName(int offset)
{
    if (!isValidLastTouchedOffset(offset))
        return {};

    return "Plugin Parm " + juce::String(offset + 1);
}

juce::String createLastTouchedHoldTargetToken()
{
    return juce::String(kLastTouchedHoldTargetToken.data());
}

bool isLastTouchedHoldTargetToken(const juce::String& token)
{
    return token.equalsIgnoreCase(kLastTouchedHoldTargetToken.data());
}

juce::String getLastTouchedHoldTargetDisplayName()
{
    return "Hold Last Touched History";
}

juce::String createObsFilterAudioLastTouchedTargetToken(int offset)
{
    if (!isValidLastTouchedOffset(offset))
        return {};

    return juce::String(kObsFilterAudioLastTouchedTargetPrefix.data()) + juce::String(offset);
}

bool parseObsFilterAudioLastTouchedTargetOffset(const juce::String& token, int& offset)
{
    offset = -1;

    if (!token.startsWithIgnoreCase(kObsFilterAudioLastTouchedTargetPrefix.data()))
        return false;

    const auto suffix = token.fromFirstOccurrenceOf(kObsFilterAudioLastTouchedTargetPrefix.data(), false, false).trim();
    if (suffix.isEmpty() || !suffix.containsOnly("0123456789"))
        return false;

    const auto parsedOffset = suffix.getIntValue();
    if (!isValidLastTouchedOffset(parsedOffset))
        return false;

    offset = parsedOffset;
    return true;
}

juce::String getObsFilterAudioLastTouchedTargetDisplayName(int offset)
{
    if (!isValidLastTouchedOffset(offset))
        return {};

    return "Audio Parm " + juce::String(offset + 1);
}

juce::String createObsFilterVideoLastTouchedTargetToken(int offset)
{
    if (!isValidLastTouchedOffset(offset))
        return {};

    return juce::String(kObsFilterVideoLastTouchedTargetPrefix.data()) + juce::String(offset);
}

bool parseObsFilterVideoLastTouchedTargetOffset(const juce::String& token, int& offset)
{
    offset = -1;

    if (!token.startsWithIgnoreCase(kObsFilterVideoLastTouchedTargetPrefix.data()))
        return false;

    const auto suffix = token.fromFirstOccurrenceOf(kObsFilterVideoLastTouchedTargetPrefix.data(), false, false).trim();
    if (suffix.isEmpty() || !suffix.containsOnly("0123456789"))
        return false;

    const auto parsedOffset = suffix.getIntValue();
    if (!isValidLastTouchedOffset(parsedOffset))
        return false;

    offset = parsedOffset;
    return true;
}

juce::String getObsFilterVideoLastTouchedTargetDisplayName(int offset)
{
    if (!isValidLastTouchedOffset(offset))
        return {};

    return "Video Parm " + juce::String(offset + 1);
}

juce::String createMidiControlMappingContextTargetToken(int targetType)
{
    if (targetType == midi_control_mapping_context_target_type_bank)
        return createMappingContextToken(
            kMappingContextTargetPrefix,
            juce::String(kMappingContextTargetBankToken.data())
        );

    if (targetType == midi_control_mapping_context_target_type_preset)
        return createMappingContextToken(
            kMappingContextTargetPrefix,
            juce::String(kMappingContextTargetPresetToken.data())
        );

    return {};
}

bool parseMidiControlMappingContextTargetType(const juce::String& token, int& targetType)
{
    targetType = midi_control_mapping_context_target_type_none;

    if (!token.startsWithIgnoreCase(kMappingContextTargetPrefix.data()))
        return false;

    auto suffix = token.fromFirstOccurrenceOf(kMappingContextTargetPrefix.data(), false, false).trim();
    if (suffix.equalsIgnoreCase(kMappingContextTargetBankToken.data()))
    {
        targetType = midi_control_mapping_context_target_type_bank;
        return true;
    }

    if (suffix.equalsIgnoreCase(kMappingContextTargetPresetToken.data()))
    {
        targetType = midi_control_mapping_context_target_type_preset;
        return true;
    }

    return false;
}

juce::String getMidiControlMappingContextTargetDisplayName(int targetType)
{
    if (targetType == midi_control_mapping_context_target_type_bank)
        return "Bank";

    if (targetType == midi_control_mapping_context_target_type_preset)
        return "Preset";

    return {};
}

std::vector<MidiControlTargetOption> getMidiControlMappingContextTargetOptions()
{
    return {
        {
         createMidiControlMappingContextTargetToken(midi_control_mapping_context_target_type_bank),
         getMidiControlMappingContextTargetDisplayName(midi_control_mapping_context_target_type_bank),
         },
        {
         createMidiControlMappingContextTargetToken(midi_control_mapping_context_target_type_preset),
         getMidiControlMappingContextTargetDisplayName(midi_control_mapping_context_target_type_preset),
         },
    };
}

juce::String createMidiControlMappingContextParameterToken(int directValue)
{
    if (!isValidMappingContextDirectTargetValue(directValue))
        return {};

    return createMappingContextToken(kMappingContextParameterPrefix, juce::String(directValue));
}

juce::String createMidiControlMappingContextParameterNextToken()
{
    return createMappingContextToken(kMappingContextParameterPrefix, juce::String(kMappingTargetNextToken.data()));
}

juce::String createMidiControlMappingContextParameterPreviousToken()
{
    return createMappingContextToken(kMappingContextParameterPrefix, juce::String(kMappingTargetPreviousToken.data()));
}

bool parseMidiControlMappingContextParameterToken(const juce::String& token, int& directValue, int& relativeStep)
{
    return parseMappingContextDirectOrRelativeValueToken(
        token,
        kMappingContextParameterPrefix,
        directValue,
        relativeStep
    );
}

juce::String getMidiControlMappingContextParameterDisplayName(const juce::String& token)
{
    int directValue = 0;
    int relativeStep = 0;
    if (!parseMidiControlMappingContextParameterToken(token, directValue, relativeStep))
        return {};

    if (relativeStep > 0)
        return "Next";

    if (relativeStep < 0)
        return "Previous";

    if (directValue >= midi_control_mapping_context_min && directValue <= midi_control_mapping_context_max)
        return juce::String(directValue);

    return {};
}

std::vector<MidiControlTargetOption> getMidiControlMappingContextParameterOptions()
{
    std::vector<MidiControlTargetOption> options;

    for (int value = midi_control_mapping_context_min; value <= midi_control_mapping_context_max; ++value)
    {
        auto token = createMidiControlMappingContextParameterToken(value);
        options.push_back({token, getMidiControlMappingContextParameterDisplayName(token)});
    }

    auto nextToken = createMidiControlMappingContextParameterNextToken();
    options.push_back({nextToken, getMidiControlMappingContextParameterDisplayName(nextToken)});

    auto previousToken = createMidiControlMappingContextParameterPreviousToken();
    options.push_back({previousToken, getMidiControlMappingContextParameterDisplayName(previousToken)});

    return options;
}

juce::String createMidiControlHotkeyIdTargetToken(uint64_t hotkeyId)
{
    return juce::String(kHotkeyIdTargetPrefix.data()) + juce::String((long long)hotkeyId);
}

bool parseMidiControlHotkeyTargetToken(const juce::String& token, MidiControlHotkeyTarget& target)
{
    target = MidiControlHotkeyTarget{};

    auto normalizedToken = token.trim();
    if (!normalizedToken.startsWithIgnoreCase(kHotkeyTargetPrefix.data()))
        return false;

    if (!normalizedToken.startsWithIgnoreCase(kHotkeyIdTargetPrefix.data()))
        return false;

    auto idSuffix = normalizedToken.fromFirstOccurrenceOf(kHotkeyIdTargetPrefix.data(), false, false).trim();
    if (idSuffix.isEmpty() || !idSuffix.containsOnly("0123456789"))
        return false;

    auto parsedValue = idSuffix.getLargeIntValue();
    if (parsedValue < 0)
        return false;

    target.targetType = midi_control_hotkey_target_type_frontend;
    target.hotkeyId = uint64_t(parsedValue);
    return true;
}

std::vector<MidiControlActionOption> getMidiControlActionOptions()
{
    return {
        {           midi_control_action_type_none,         "None"},
        {midi_control_action_type_mapping_context,  "Bank/Preset"},
        {         midi_control_action_type_source,       "Source"},
        {    midi_control_action_type_effect_parm,  "Effect Parm"},
        {   midi_control_action_type_last_touched, "Last Touched"},
        {          midi_control_action_type_scene,        "Scene"},
        {     midi_control_action_type_transition,   "Transition"},
        {         midi_control_action_type_hotkey,       "Hotkey"},
        {          midi_control_action_type_media,        "Media"},
    };
}

std::vector<MidiControlTargetOption> getMidiControlTargetOptionsForAction(int actionType)
{
    std::vector<MidiControlTargetOption> options;

    if (actionType == midi_control_action_type_last_touched)
    {
        options.push_back({createLastTouchedHoldTargetToken(), getLastTouchedHoldTargetDisplayName()});

        for (int offset = 0; offset <= kMaxLastTouchedOffset; ++offset)
            options.push_back({createLastTouchedTargetToken(offset), getLastTouchedTargetDisplayName(offset)});

        for (int offset = 0; offset <= kMaxLastTouchedOffset; ++offset)
            options.push_back({
                createObsFilterAudioLastTouchedTargetToken(offset),
                getObsFilterAudioLastTouchedTargetDisplayName(offset),
            });

        for (int offset = 0; offset <= kMaxLastTouchedOffset; ++offset)
            options.push_back({
                createObsFilterVideoLastTouchedTargetToken(offset),
                getObsFilterVideoLastTouchedTargetDisplayName(offset),
            });

        return options;
    }

    if (actionType == midi_control_action_type_mapping_context)
    {
        options = getMidiControlMappingContextTargetOptions();
        return options;
    }

    if (actionType == midi_control_action_type_hotkey)
        return options;

    return options;
}

juce::String createMidiControlMediaActionTypeLayerToken(int mediaActionType)
{
    if (mediaActionType == midi_control_media_action_type_play_pause)
        return juce::String(kMediaActionLayerPrefix.data()) + juce::String(kMediaActionPlayPauseToken.data());

    if (mediaActionType == midi_control_media_action_type_restart)
        return juce::String(kMediaActionLayerPrefix.data()) + juce::String(kMediaActionRestartToken.data());

    if (mediaActionType == midi_control_media_action_type_stop)
        return juce::String(kMediaActionLayerPrefix.data()) + juce::String(kMediaActionStopToken.data());

    return {};
}

bool parseMidiControlMediaActionType(const juce::String& token, int& mediaActionType)
{
    mediaActionType = midi_control_media_action_type_none;

    if (!token.startsWithIgnoreCase(kMediaActionLayerPrefix.data()))
        return false;

    auto suffix = token.fromFirstOccurrenceOf(kMediaActionLayerPrefix.data(), false, false).trim();
    if (suffix.equalsIgnoreCase(kMediaActionPlayPauseToken.data()))
    {
        mediaActionType = midi_control_media_action_type_play_pause;
        return true;
    }

    if (suffix.equalsIgnoreCase(kMediaActionRestartToken.data()))
    {
        mediaActionType = midi_control_media_action_type_restart;
        return true;
    }

    if (suffix.equalsIgnoreCase(kMediaActionStopToken.data()))
    {
        mediaActionType = midi_control_media_action_type_stop;
        return true;
    }

    return false;
}

juce::String getMidiControlMediaActionTypeDisplayName(int mediaActionType)
{
    if (mediaActionType == midi_control_media_action_type_play_pause)
        return "Play/Pause";

    if (mediaActionType == midi_control_media_action_type_restart)
        return "Restart";

    if (mediaActionType == midi_control_media_action_type_stop)
        return "Stop";

    return {};
}

std::vector<MidiControlTargetOption> getMidiControlMediaActionTypeOptions()
{
    return {
        {
         createMidiControlMediaActionTypeLayerToken(midi_control_media_action_type_play_pause),
         getMidiControlMediaActionTypeDisplayName(midi_control_media_action_type_play_pause),
         },
        {
         createMidiControlMediaActionTypeLayerToken(midi_control_media_action_type_restart),
         getMidiControlMediaActionTypeDisplayName(midi_control_media_action_type_restart),
         },
        {
         createMidiControlMediaActionTypeLayerToken(midi_control_media_action_type_stop),
         getMidiControlMediaActionTypeDisplayName(midi_control_media_action_type_stop),
         },
    };
}

juce::String createMidiControlSourceActionTypeLayerToken(int sourceActionType)
{
    if (sourceActionType == midi_control_source_action_type_volume)
        return juce::String(kSourceActionLayerPrefix.data()) + juce::String(kSourceActionVolumeToken.data());

    if (sourceActionType == midi_control_source_action_type_mute)
        return juce::String(kSourceActionLayerPrefix.data()) + juce::String(kSourceActionMuteToken.data());

    if (sourceActionType == midi_control_source_action_type_monitoring)
        return juce::String(kSourceActionLayerPrefix.data()) + juce::String(kSourceActionMonitoringToken.data());

    return {};
}

bool parseMidiControlSourceActionType(const juce::String& token, int& sourceActionType)
{
    sourceActionType = midi_control_source_action_type_none;

    if (!token.startsWithIgnoreCase(kSourceActionLayerPrefix.data()))
        return false;

    auto suffix = token.fromFirstOccurrenceOf(kSourceActionLayerPrefix.data(), false, false).trim();
    if (suffix.equalsIgnoreCase(kSourceActionVolumeToken.data()))
    {
        sourceActionType = midi_control_source_action_type_volume;
        return true;
    }

    if (suffix.equalsIgnoreCase(kSourceActionMuteToken.data()))
    {
        sourceActionType = midi_control_source_action_type_mute;
        return true;
    }

    if (suffix.equalsIgnoreCase(kSourceActionMonitoringToken.data()))
    {
        sourceActionType = midi_control_source_action_type_monitoring;
        return true;
    }

    return false;
}

juce::String getMidiControlSourceActionTypeDisplayName(int sourceActionType)
{
    if (sourceActionType == midi_control_source_action_type_volume)
        return "Volume";

    if (sourceActionType == midi_control_source_action_type_mute)
        return "Mute";

    if (sourceActionType == midi_control_source_action_type_monitoring)
        return "Monitoring";

    return {};
}

std::vector<MidiControlTargetOption> getMidiControlSourceActionTypeOptions()
{
    return {
        {
         createMidiControlSourceActionTypeLayerToken(midi_control_source_action_type_volume),
         getMidiControlSourceActionTypeDisplayName(midi_control_source_action_type_volume),
         },
        {
         createMidiControlSourceActionTypeLayerToken(midi_control_source_action_type_mute),
         getMidiControlSourceActionTypeDisplayName(midi_control_source_action_type_mute),
         },
        {
         createMidiControlSourceActionTypeLayerToken(midi_control_source_action_type_monitoring),
         getMidiControlSourceActionTypeDisplayName(midi_control_source_action_type_monitoring),
         },
    };
}

bool validateMidiControlMapping(const MidiControlMapping& mapping, juce::String& errorMessage)
{
    if (mapping.mappingId.isEmpty())
    {
        errorMessage = "mappingId is empty";
        return false;
    }

    if (mapping.actionType < midi_control_action_type_none || mapping.actionType > midi_control_action_type_effect_parm)
    {
        errorMessage = "invalid actionType in mapping " + mapping.mappingId;
        return false;
    }

    if (!isValidMappingContextValue(mapping.bank))
    {
        errorMessage = "invalid bank in mapping " + mapping.mappingId;
        return false;
    }

    if (!isValidMappingContextValue(mapping.preset))
    {
        errorMessage = "invalid preset in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.messageType < midi_control_message_type_cc
        || mapping.messageType > midi_control_message_type_program_change)
    {
        errorMessage = "invalid messageType in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.interactionMode < midi_control_interaction_mode_absolute
        || mapping.interactionMode > midi_control_interaction_mode_inc_dec2)
    {
        errorMessage = "invalid interactionMode in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.outputMode < midi_control_output_mode_none || mapping.outputMode > midi_control_output_mode_specific)
    {
        errorMessage = "invalid outputMode in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.activationMode < midi_control_activation_mode_scene
        || mapping.activationMode > midi_control_activation_mode_preview)
    {
        errorMessage = "invalid activationMode in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.channel < 0 || mapping.channel > 16)
    {
        errorMessage = "invalid channel in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.data1 < 0 || mapping.data1 > 127)
    {
        errorMessage = "invalid data1 in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.maxValue < mapping.minValue)
    {
        errorMessage = "min/max range is invalid in mapping " + mapping.mappingId;
        return false;
    }

    if (!std::isfinite(mapping.scale) || mapping.scale < 0.0)
    {
        errorMessage = "scale is invalid in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.outputMode == midi_control_output_mode_specific && mapping.outputDeviceName.isEmpty())
    {
        errorMessage = "specific outputMode without outputDeviceName in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.actionType == midi_control_action_type_none)
        return true;

    if (mapping.actionType == midi_control_action_type_scene)
    {
        if (mapping.targetName.isEmpty())
        {
            errorMessage = "scene mapping targetName is empty in mapping " + mapping.mappingId;
            return false;
        }

        return true;
    }

    if (mapping.actionType == midi_control_action_type_transition)
    {
        if (mapping.targetName.isEmpty())
        {
            errorMessage = "transition mapping targetName is empty in mapping " + mapping.mappingId;
            return false;
        }

        return true;
    }

    if (mapping.actionType == midi_control_action_type_mapping_context)
    {
        int targetType = midi_control_mapping_context_target_type_none;
        if (!parseMidiControlMappingContextTargetType(mapping.targetUuid, targetType))
        {
            errorMessage = "mapping context target is invalid in mapping " + mapping.mappingId;
            return false;
        }

        int directValue = 0;
        int relativeStep = 0;
        if (!parseMidiControlMappingContextParameterToken(mapping.targetParameterToken, directValue, relativeStep))
        {
            errorMessage = "mapping context parameter is invalid in mapping " + mapping.mappingId;
            return false;
        }

        if (relativeStep == 0 && !isValidMappingContextDirectTargetValue(directValue))
        {
            errorMessage = "mapping context parameter is out of range in mapping " + mapping.mappingId;
            return false;
        }

        return true;
    }

    if (mapping.actionType == midi_control_action_type_last_touched)
    {
        if (isLastTouchedHoldTargetToken(mapping.targetUuid))
            return true;

        int offset = -1;
        if (parseLastTouchedTargetOffset(mapping.targetUuid, offset))
            return true;

        if (parseObsFilterAudioLastTouchedTargetOffset(mapping.targetUuid, offset))
            return true;

        if (parseObsFilterVideoLastTouchedTargetOffset(mapping.targetUuid, offset))
            return true;

        errorMessage = "last touched target is invalid in mapping " + mapping.mappingId;
        return false;
    }

    if (mapping.actionType == midi_control_action_type_hotkey)
    {
        MidiControlHotkeyTarget target;
        if (!parseMidiControlHotkeyTargetToken(mapping.targetUuid, target))
        {
            errorMessage = "hotkey target is invalid in mapping " + mapping.mappingId;
            return false;
        }

        if (mapping.outputMode != midi_control_output_mode_none)
        {
            errorMessage = "hotkey mapping must use output route none in mapping " + mapping.mappingId;
            return false;
        }

        return true;
    }

    if (mapping.actionType == midi_control_action_type_effect_parm)
    {
        if (mapping.targetUuid.isEmpty())
        {
            errorMessage = "effect parm mapping targetUuid is empty in mapping " + mapping.mappingId;
            return false;
        }

        if (mapping.targetLayerToken.isEmpty())
        {
            errorMessage = "effect parm mapping targetLayerToken is empty in mapping " + mapping.mappingId;
            return false;
        }

        if (mapping.targetParameterToken.isEmpty())
        {
            errorMessage = "effect parm mapping targetParameterToken is empty in mapping " + mapping.mappingId;
            return false;
        }

        return true;
    }

    if (mapping.actionType == midi_control_action_type_source)
    {
        if (mapping.targetUuid.isEmpty())
        {
            errorMessage = "source mapping targetUuid is empty in mapping " + mapping.mappingId;
            return false;
        }

        int sourceActionType = midi_control_source_action_type_none;
        if (!parseMidiControlSourceActionType(mapping.targetLayerToken, sourceActionType))
        {
            errorMessage = "source mapping targetLayerToken is invalid in mapping " + mapping.mappingId;
            return false;
        }

        return true;
    }

    if (mapping.actionType == midi_control_action_type_media)
    {
        if (mapping.targetUuid.isEmpty())
        {
            errorMessage = "media mapping targetUuid is empty in mapping " + mapping.mappingId;
            return false;
        }

        int mediaActionType = midi_control_media_action_type_none;
        if (!parseMidiControlMediaActionType(mapping.targetLayerToken, mediaActionType))
        {
            errorMessage = "media mapping targetLayerToken is invalid in mapping " + mapping.mappingId;
            return false;
        }

        return true;
    }

    if (mapping.targetUuid.isEmpty())
    {
        errorMessage = "targetUuid is empty in mapping " + mapping.mappingId;
        return false;
    }

    return true;
}

bool validateMidiControlMappings(const std::vector<MidiControlMapping>& mappings, juce::String& errorMessage)
{
    for (auto& mapping : mappings)
        if (!validateMidiControlMapping(mapping, errorMessage))
            return false;

    return true;
}

juce::String createMidiControlMappingId()
{
    return juce::Uuid().toString();
}

juce::String serializeMidiControlMappings(const std::vector<MidiControlMapping>& mappings)
{
    return juce::JSON::toString(createMappingsArrayVar(mappings));
}

bool deserializeMidiControlMappings(const juce::String& data, std::vector<MidiControlMapping>& mappings)
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
    {
        if (item.getDynamicObject() == nullptr)
            return false;

        auto* object = item.getDynamicObject();
        if (object == nullptr)
            return false;

        if (!object->hasProperty("targetLayerToken") || !object->hasProperty("targetParameterToken"))
            return false;

        mappings.push_back(deserializeMapping(item));
    }

    juce::String validationError;
    if (!validateMidiControlMappings(mappings, validationError))
    {
        mappings.clear();
        return false;
    }

    return true;
}

juce::String serializeMidiControlMappingsFilePayload(const std::vector<MidiControlMapping>& mappings)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("format", juce::String(kMappingsFilePayloadFormat.data()));
    object->setProperty("version", kMappingsFilePayloadVersion);
    object->setProperty("mappings", createMappingsArrayVar(mappings));
    return juce::JSON::toString(juce::var(object));
}

bool deserializeMidiControlMappingsFilePayload(
    const juce::String& data,
    std::vector<MidiControlMapping>& mappings,
    juce::String& errorMessage
)
{
    mappings.clear();
    errorMessage.clear();

    if (data.trim().isEmpty())
    {
        errorMessage = "mapping file is empty";
        return false;
    }

    auto parsed = juce::JSON::parse(data);

    auto* object = parsed.getDynamicObject();
    if (object == nullptr)
    {
        errorMessage = "mapping file must contain a JSON object";
        return false;
    }

    const auto format = object->getProperty("format").toString();
    if (!format.equalsIgnoreCase(kMappingsFilePayloadFormat.data()))
    {
        errorMessage = "unsupported mapping file format";
        return false;
    }

    const auto version = int(object->getProperty("version"));
    if (version != kMappingsFilePayloadVersion)
    {
        errorMessage = "unsupported mapping file version";
        return false;
    }

    const auto mappingsValue = object->getProperty("mappings");
    if (!mappingsValue.isArray())
    {
        errorMessage = "mapping file does not contain a mappings array";
        return false;
    }

    if (!deserializeMidiControlMappings(juce::JSON::toString(mappingsValue), mappings))
    {
        errorMessage = "mapping list is invalid";
        return false;
    }

    return true;
}

} // namespace atk
