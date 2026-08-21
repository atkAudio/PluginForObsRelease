#include "midi_control_controller.h"

#include <atkaudio/GlobalSettings.h>
#include <atkaudio/Logging.h>
#include <atkaudio/midi_control/last_touched_parameter_tracker.h>
#include <atkaudio/midi_control/midi_control_active_context_dock.h>
#include <atkaudio/midi_control/midi_control_dialog.h>
#include <atkaudio/midi_control/midi_control_last_touched_helper_dialog.h>
#include <atkaudio/midi_control/obs_filter_last_touched_tracker.h>

#include <obs-frontend-api.h>
#include <obs-audio-controls.h>
#include <obs-hotkey.h>
#include <util/platform.h>

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string_view>

namespace atk
{

namespace
{
MidiControlController* g_midiObsController = nullptr;

constexpr double feedbackVolumeThreshold = 1.0 / 127.0;
constexpr double feedbackSuppressionWindowMs = 150.0;
constexpr int delayedFeedbackOutputMinIdleMs = 50;
constexpr int delayedFeedbackOutputMaxIdleMs = 5000;
constexpr int tbarMaxPosition = 1023;
constexpr int tbarZeroThreshold = 1;
constexpr int tbarCompleteThreshold = tbarMaxPosition - 1;
constexpr auto tbarObjectName = "tBar";

constexpr auto previousSceneTargetName = "Previous Scene";
constexpr auto nextSceneTargetName = "Next Scene";
constexpr int mappingActiveContextMin = midi_control_mapping_context_min;
constexpr int mappingActiveContextMax = midi_control_mapping_context_max;

constexpr std::string_view effectParmObsFilterLayerPrefix = "effect_parm_obs_filter:";
constexpr std::string_view effectParmIndexPrefix = "effect_parm_index:";
constexpr std::string_view pluginHostFilterId = "atkaudio_plugin_host";
constexpr std::string_view pluginHost2FilterId = "atkaudio_plugin_host2";
constexpr const char* kDefaultHistoryCollectionId = "default";

int wrapMappingContextValue(int value)
{
    auto range = mappingActiveContextMax - mappingActiveContextMin + 1;
    if (range <= 0)
        return mappingActiveContextMin;

    auto normalized = (value - mappingActiveContextMin) % range;
    if (normalized < 0)
        normalized += range;

    return normalized + mappingActiveContextMin;
}

[[noreturn]] void failFast(const char* context, juce::String message)
{
    atk::logging::error(context, "Fatal: " + message);
    std::abort();
}

juce::String getCurrentSceneCollectionId()
{
    auto* sceneCollection = obs_frontend_get_current_scene_collection();
    if (sceneCollection == nullptr)
        return kDefaultHistoryCollectionId;

    auto collectionId = juce::String(sceneCollection).trim();
    bfree(sceneCollection);

    if (collectionId.isEmpty())
        return kDefaultHistoryCollectionId;

    return collectionId;
}

void setLastTouchedPersistenceSuspended(bool suspended)
{
    LastTouchedParameterTracker::getInstance().setPersistenceSuspended(suspended);
    ObsFilterLastTouchedTracker::getInstance().setPersistenceSuspended(suspended);
}

void setLastTouchedActiveCollection(const juce::String& collectionId)
{
    LastTouchedParameterTracker::getInstance().setActiveCollectionId(collectionId);
    ObsFilterLastTouchedTracker::getInstance().setActiveCollectionId(collectionId);
}

bool isSceneSource(obs_source_t* source)
{
    return source != nullptr && obs_scene_from_source(source) != nullptr;
}

obs_source_t* getSceneSource()
{
    return obs_frontend_get_current_scene();
}

obs_source_t* getPreviewSceneSource()
{
    if (obs_frontend_preview_program_mode_active())
    {
        auto* previewScene = obs_frontend_get_current_preview_scene();
        if (previewScene != nullptr)
            return previewScene;
    }

    return obs_frontend_get_current_scene();
}

obs_source_t* getSceneSourceForMapping(const MidiControlMapping& mapping)
{
    if (mapping.activationMode == midi_control_activation_mode_preview)
        return getPreviewSceneSource();

    return getSceneSource();
}

bool isMappingGlobal(const MidiControlMapping& mapping)
{
    return mapping.activationMode == midi_control_activation_mode_global;
}

struct SceneMembershipSearch
{
    juce::String targetUuid;
    bool found = false;
};

bool sceneContainsSourceRecursive(obs_scene_t* scene, const juce::String& targetUuid);

bool sceneContainsSourceItem(obs_scene_t*, obs_sceneitem_t* item, void* privateData)
{
    auto* search = static_cast<SceneMembershipSearch*>(privateData);
    if (search == nullptr || item == nullptr)
        return true;

    auto* itemSource = obs_sceneitem_get_source(item);
    if (itemSource == nullptr)
        return true;

    auto* itemUuid = obs_source_get_uuid(itemSource);
    if (itemUuid != nullptr && search->targetUuid == itemUuid)
    {
        search->found = true;
        return false;
    }

    auto* childScene = obs_scene_from_source(itemSource);
    if (childScene != nullptr && sceneContainsSourceRecursive(childScene, search->targetUuid))
    {
        search->found = true;
        return false;
    }

    return true;
}

bool sceneContainsSourceRecursive(obs_scene_t* scene, const juce::String& targetUuid)
{
    if (scene == nullptr || targetUuid.isEmpty())
        return false;

    SceneMembershipSearch search;
    search.targetUuid = targetUuid;
    obs_scene_enum_items(scene, sceneContainsSourceItem, &search);
    return search.found;
}

bool sourceExistsInAnyScene(const juce::String& targetUuid)
{
    if (targetUuid.isEmpty())
        return false;

    obs_frontend_source_list sceneList = {};
    obs_frontend_get_scenes(&sceneList);

    bool found = false;

    for (size_t i = 0; i < sceneList.sources.num; ++i)
    {
        auto* sceneSource = sceneList.sources.array[i];
        if (sceneSource == nullptr)
            continue;

        auto* scene = obs_scene_from_source(sceneSource);
        if (scene != nullptr && sceneContainsSourceRecursive(scene, targetUuid))
        {
            found = true;
            break;
        }
    }

    obs_frontend_source_list_free(&sceneList);
    return found;
}

bool isSourceActiveForMapping(const MidiControlMapping& mapping, obs_source_t* source)
{
    if (source == nullptr)
        return false;

    if (isMappingGlobal(mapping))
        return true;

    auto* sourceUuid = obs_source_get_uuid(source);
    if (sourceUuid == nullptr || sourceUuid[0] == '\0')
        return false;

    auto targetUuid = juce::String(sourceUuid);
    if (!sourceExistsInAnyScene(targetUuid))
        return true;

    auto* currentSceneSource = getSceneSourceForMapping(mapping);
    if (currentSceneSource == nullptr)
        return false;

    auto* currentScene = obs_scene_from_source(currentSceneSource);
    auto isContained = sceneContainsSourceRecursive(currentScene, targetUuid);
    obs_source_release(currentSceneSource);
    return isContained;
}

struct ResolvedTargetSource
{
    obs_source_t* source = nullptr;
    juce::String resolvedUuid;
    bool usedNameFallback = false;
};

struct HotkeyIdLookupContext
{
    obs_hotkey_id wantedId = OBS_INVALID_HOTKEY_ID;
    bool isFrontend = false;
};

bool isFrontendHotkeyIdFromObs(obs_hotkey_id hotkeyId)
{
    HotkeyIdLookupContext lookupContext;
    lookupContext.wantedId = hotkeyId;

    obs_enum_hotkeys(
        [](void* privateData, obs_hotkey_id id, obs_hotkey_t* key)
        {
            auto* context = static_cast<HotkeyIdLookupContext*>(privateData);
            if (context == nullptr || key == nullptr)
                return true;

            if (id != context->wantedId)
                return true;

            context->isFrontend = obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_FRONTEND;
            return false;
        },
        &lookupContext
    );

    return lookupContext.isFrontend;
}

obs_hotkey_id resolveHotkeyIdFromMappingTarget(const MidiControlMapping& mapping)
{
    MidiControlHotkeyTarget target;
    if (!parseMidiControlHotkeyTargetToken(mapping.targetUuid, target))
        return OBS_INVALID_HOTKEY_ID;

    auto hotkeyId = obs_hotkey_id(target.hotkeyId);
    return isFrontendHotkeyIdFromObs(hotkeyId) ? hotkeyId : OBS_INVALID_HOTKEY_ID;
}

juce::String createEffectParmLayerToken(juce::String filterUuid)
{
    if (filterUuid.isEmpty())
        return {};

    return juce::String(effectParmObsFilterLayerPrefix.data()) + filterUuid;
}

obs_source_t* findSourceByNameExactMatch(juce::String targetName)
{
    if (targetName.isEmpty())
        return nullptr;

    return obs_get_source_by_name(targetName.toRawUTF8());
}

ResolvedTargetSource resolveTargetSource(const MidiControlMapping& mapping, bool matchByNameEnabled)
{
    ResolvedTargetSource result;

    if (mapping.targetUuid.isNotEmpty())
    {
        result.source = obs_get_source_by_uuid(mapping.targetUuid.toRawUTF8());
        if (result.source != nullptr)
        {
            auto* sourceUuid = obs_source_get_uuid(result.source);
            if (sourceUuid != nullptr)
                result.resolvedUuid = sourceUuid;

            return result;
        }
    }

    if (!matchByNameEnabled || mapping.targetName.isEmpty())
        return result;

    result.source = findSourceByNameExactMatch(mapping.targetName);
    if (result.source == nullptr)
        return result;

    auto* sourceUuid = obs_source_get_uuid(result.source);
    if (sourceUuid == nullptr || sourceUuid[0] == '\0')
    {
        obs_source_release(result.source);
        result.source = nullptr;
        return result;
    }

    result.resolvedUuid = sourceUuid;
    result.usedNameFallback = true;
    return result;
}

bool isFeedbackMappingActiveForMapping(const MidiControlMapping& mapping, bool matchByNameEnabled)
{
    if (mapping.actionType == midi_control_action_type_mapping_context)
        return true;

    if (mapping.actionType == midi_control_action_type_last_touched)
    {
        if (isLastTouchedHoldTargetToken(mapping.targetUuid))
            return true;

        int offset = -1;
        if (parseLastTouchedTargetOffset(mapping.targetUuid, offset))
        {
            LastTouchedParameterEntry parameterEntry;
            if (!LastTouchedParameterTracker::getInstance().getParameterAtOffset(offset, parameterEntry))
                return false;

            if (mapping.activationMode != midi_control_activation_mode_global)
            {
                if (parameterEntry.ownerSourceUuid.isEmpty())
                    return false;

                auto* ownerSource = obs_get_source_by_uuid(parameterEntry.ownerSourceUuid.toRawUTF8());
                if (ownerSource == nullptr)
                    return false;

                auto isActive = isSourceActiveForMapping(mapping, ownerSource);
                obs_source_release(ownerSource);
                if (!isActive)
                    return false;
            }

            return parameterEntry.parameter != nullptr;
        }

        if (parseObsFilterAudioLastTouchedTargetOffset(mapping.targetUuid, offset))
        {
            ObsFilterLastTouchedEntry parameterEntry;
            if (!ObsFilterLastTouchedTracker::getInstance().getAudioParameterAtOffset(offset, parameterEntry))
                return false;

            if (mapping.activationMode != midi_control_activation_mode_global)
            {
                if (parameterEntry.sourceUuid.isEmpty())
                    return false;

                auto* ownerSource = obs_get_source_by_uuid(parameterEntry.sourceUuid.toRawUTF8());
                if (ownerSource == nullptr)
                    return false;

                auto isActive = isSourceActiveForMapping(mapping, ownerSource);
                obs_source_release(ownerSource);
                if (!isActive)
                    return false;
            }

            return true;
        }

        if (parseObsFilterVideoLastTouchedTargetOffset(mapping.targetUuid, offset))
        {
            ObsFilterLastTouchedEntry parameterEntry;
            if (!ObsFilterLastTouchedTracker::getInstance().getVideoParameterAtOffset(offset, parameterEntry))
                return false;

            if (mapping.activationMode != midi_control_activation_mode_global)
            {
                if (parameterEntry.sourceUuid.isEmpty())
                    return false;

                auto* ownerSource = obs_get_source_by_uuid(parameterEntry.sourceUuid.toRawUTF8());
                if (ownerSource == nullptr)
                    return false;

                auto isActive = isSourceActiveForMapping(mapping, ownerSource);
                obs_source_release(ownerSource);
                if (!isActive)
                    return false;
            }

            return true;
        }

        return false;
    }

    if (mapping.actionType != midi_control_action_type_source)
        return false;

    int sourceActionType = midi_control_source_action_type_none;
    if (!parseMidiControlSourceActionType(mapping.targetLayerToken, sourceActionType))
        return false;

    auto sourceResult = resolveTargetSource(mapping, matchByNameEnabled);
    auto* source = sourceResult.source;
    if (source == nullptr)
        return false;

    auto isActive = isSourceActiveForMapping(mapping, source);
    obs_source_release(source);
    return isActive;
}

bool isLastTouchedEntryActiveForMapping(
    const MidiControlMapping& mapping,
    const LastTouchedParameterEntry& parameterEntry
)
{
    if (isMappingGlobal(mapping))
        return true;

    if (parameterEntry.ownerSourceUuid.isEmpty())
        return false;

    auto* ownerSource = obs_get_source_by_uuid(parameterEntry.ownerSourceUuid.toRawUTF8());
    if (ownerSource == nullptr)
        return false;

    auto isActive = isSourceActiveForMapping(mapping, ownerSource);
    obs_source_release(ownerSource);
    return isActive;
}

bool isObsFilterLastTouchedEntryActiveForMapping(
    const MidiControlMapping& mapping,
    const ObsFilterLastTouchedEntry& parameterEntry
)
{
    if (isMappingGlobal(mapping))
        return true;

    if (parameterEntry.sourceUuid.isEmpty())
        return false;

    auto* ownerSource = obs_get_source_by_uuid(parameterEntry.sourceUuid.toRawUTF8());
    if (ownerSource == nullptr)
        return false;

    auto isActive = isSourceActiveForMapping(mapping, ownerSource);
    obs_source_release(ownerSource);
    return isActive;
}

bool hasActiveTbarState(const MidiControlTransitionState& transitionState)
{
    return transitionState.lastPosition > 0 || transitionState.releasePending || transitionState.lockedUntilZero;
}

struct EffectParmSelection
{
    juce::String filterUuid;
    juce::String parameterKey;
};

bool parseEffectParmObsFilterSelection(const MidiControlMapping& mapping, EffectParmSelection& selection)
{
    selection = {};

    if (!mapping.targetLayerToken.startsWithIgnoreCase(effectParmObsFilterLayerPrefix.data()))
        return false;

    selection.filterUuid =
        mapping.targetLayerToken.fromFirstOccurrenceOf(effectParmObsFilterLayerPrefix.data(), false, false).trim();
    selection.parameterKey = mapping.targetParameterToken.trim();

    if (selection.filterUuid.isEmpty() || selection.parameterKey.isEmpty())
        return false;

    return true;
}

bool parsePluginParameterIndexToken(const juce::String& token, int& parameterIndex)
{
    parameterIndex = -1;

    if (!token.startsWithIgnoreCase(effectParmIndexPrefix.data()))
        return false;

    auto suffix = token.fromFirstOccurrenceOf(effectParmIndexPrefix.data(), false, false).trim();
    if (suffix.isEmpty() || !suffix.containsOnly("0123456789"))
        return false;

    auto parsed = suffix.getIntValue();
    if (parsed < 0)
        return false;

    parameterIndex = parsed;
    return true;
}

bool isPluginHostFilter(obs_source_t* filter)
{
    if (filter == nullptr)
        return false;

    auto* filterId = obs_source_get_id(filter);
    if (filterId == nullptr)
        return false;

    auto id = juce::String(filterId);
    return id.equalsIgnoreCase(pluginHostFilterId.data()) || id.equalsIgnoreCase(pluginHost2FilterId.data());
}

bool isWritableFilterProperty(obs_property_t* property);

uint32_t toObsFilterLaneFlags(uint32_t outputFlags)
{
    auto laneFlags = uint32_t(obs_filter_last_touched_lane_none);

    if ((outputFlags & OBS_SOURCE_AUDIO) != 0)
        laneFlags |= obs_filter_last_touched_lane_audio;

    if ((outputFlags & OBS_SOURCE_VIDEO) != 0)
        laneFlags |= obs_filter_last_touched_lane_video;

    return laneFlags;
}

bool buildObsFilterRateLimitedEntry(
    const MidiControlMapping& mapping,
    obs_source_t* filter,
    juce::String parameterKey,
    ObsFilterLastTouchedEntry& entry
)
{
    entry = {};

    if (filter == nullptr || mapping.targetUuid.isEmpty() || parameterKey.isEmpty())
        return false;

    auto laneFlags = toObsFilterLaneFlags(obs_source_get_output_flags(filter));
    if (laneFlags == obs_filter_last_touched_lane_none)
        return false;

    auto filterUuid = juce::String(obs_source_get_uuid(filter));
    if (filterUuid.isEmpty())
        return false;

    auto* properties = obs_source_properties(filter);
    if (properties == nullptr)
        return false;

    auto* property = obs_properties_get(properties, parameterKey.toRawUTF8());
    if (property == nullptr || !isWritableFilterProperty(property))
    {
        obs_properties_destroy(properties);
        return false;
    }

    auto valueType = obs_filter_last_touched_value_type_float;
    auto propertyType = obs_property_get_type(property);

    if (propertyType == OBS_PROPERTY_BOOL)
        valueType = obs_filter_last_touched_value_type_boolean;
    else if (propertyType == OBS_PROPERTY_INT)
        valueType = obs_filter_last_touched_value_type_integer;
    else if (propertyType == OBS_PROPERTY_FLOAT)
        valueType = obs_filter_last_touched_value_type_float;
    else if (propertyType == OBS_PROPERTY_LIST)
        valueType = obs_filter_last_touched_value_type_list_index;
    else
    {
        obs_properties_destroy(properties);
        return false;
    }

    obs_properties_destroy(properties);

    entry.sourceUuid = mapping.targetUuid;
    entry.filterUuid = filterUuid;
    entry.parameterKey = parameterKey;
    entry.identity = entry.sourceUuid + "::" + entry.filterUuid + "::" + entry.parameterKey;
    entry.valueType = valueType;
    entry.laneFlags = laneFlags;
    return true;
}

bool getPluginParameterEntryForSelection(
    const MidiControlMapping& mapping,
    obs_source_t* filter,
    juce::String parameterToken,
    LastTouchedParameterEntry& entry
)
{
    if (filter == nullptr || parameterToken.isEmpty() || mapping.targetUuid.isEmpty())
        return false;

    auto* filterNameText = obs_source_get_name(filter);
    if (filterNameText == nullptr || filterNameText[0] == '\0')
        return false;

    int parameterIndex = -1;
    if (!parsePluginParameterIndexToken(parameterToken, parameterIndex))
        return false;

    return LastTouchedParameterTracker::getInstance()
        .getParameterForOwner(mapping.targetUuid, juce::String(filterNameText), parameterIndex, entry);
}

bool readPluginParameterNormalizedValue(
    const MidiControlMapping& mapping,
    obs_source_t* filter,
    juce::String parameterToken,
    double& normalizedValue
)
{
    normalizedValue = 0.0;

    LastTouchedParameterEntry entry;
    if (!getPluginParameterEntryForSelection(mapping, filter, parameterToken, entry))
        return false;

    if (entry.parameter == nullptr)
        return false;

    normalizedValue = juce::jlimit(0.0, 1.0, double(entry.parameter->getValue()));
    return true;
}

bool writePluginParameterNormalizedValue(
    const MidiControlMapping& mapping,
    obs_source_t* filter,
    juce::String parameterToken,
    double normalizedValue
)
{
    LastTouchedParameterEntry entry;
    if (!getPluginParameterEntryForSelection(mapping, filter, parameterToken, entry))
        return false;

    if (entry.parameter == nullptr)
        return false;

    auto clamped = juce::jlimit(0.0f, 1.0f, float(normalizedValue));

    auto& tracker = LastTouchedParameterTracker::getInstance();
    tracker.beginInternalWrite(*entry.parameter);
    entry.parameter->setValueNotifyingHost(clamped);
    tracker.endInternalWrite(*entry.parameter);
    return true;
}

obs_source_t* findFilterByUuidOnSource(obs_source_t* source, juce::String filterUuid)
{
    if (source == nullptr || filterUuid.isEmpty())
        return nullptr;

    struct FilterLookup
    {
        juce::String wantedUuid;
        obs_source_t* filter = nullptr;
    } lookup;

    lookup.wantedUuid = filterUuid;

    obs_source_enum_filters(
        source,
        [](obs_source_t*, obs_source_t* filter, void* privateData)
        {
            auto* lookup = static_cast<FilterLookup*>(privateData);
            if (lookup == nullptr || filter == nullptr)
                return;

            auto* uuid = obs_source_get_uuid(filter);
            if (uuid == nullptr)
                return;

            if (lookup->wantedUuid != uuid)
                return;

            lookup->filter = obs_source_get_ref(filter);
        },
        &lookup
    );

    return lookup.filter;
}

obs_source_t* findFilterByNameOnSource(obs_source_t* source, juce::String filterName)
{
    if (source == nullptr || filterName.isEmpty())
        return nullptr;

    return obs_source_get_filter_by_name(source, filterName.toRawUTF8());
}

bool isWritableFilterProperty(obs_property_t* property)
{
    if (property == nullptr)
        return false;

    if (!obs_property_enabled(property) || !obs_property_visible(property))
        return false;

    auto type = obs_property_get_type(property);
    if (type == OBS_PROPERTY_BOOL || type == OBS_PROPERTY_INT || type == OBS_PROPERTY_FLOAT)
        return true;

    if (type == OBS_PROPERTY_LIST)
        return obs_property_list_type(property) == OBS_COMBO_TYPE_LIST;

    return false;
}

bool readFilterPropertyNormalizedValue(obs_source_t* filter, juce::String parameterKey, double& normalizedValue)
{
    normalizedValue = 0.0;

    if (filter == nullptr || parameterKey.isEmpty())
        return false;

    auto* properties = obs_source_properties(filter);
    if (properties == nullptr)
        return false;

    auto* settings = obs_source_get_settings(filter);
    if (settings == nullptr)
    {
        obs_properties_destroy(properties);
        return false;
    }

    auto* property = obs_properties_get(properties, parameterKey.toRawUTF8());
    if (property == nullptr || !isWritableFilterProperty(property))
    {
        obs_data_release(settings);
        obs_properties_destroy(properties);
        return false;
    }

    auto key = obs_property_name(property);
    auto type = obs_property_get_type(property);

    if (type == OBS_PROPERTY_BOOL)
    {
        normalizedValue = obs_data_get_bool(settings, key) ? 1.0 : 0.0;
    }
    else if (type == OBS_PROPERTY_INT)
    {
        auto intMin = obs_property_int_min(property);
        auto intMax = obs_property_int_max(property);
        if (intMax <= intMin)
        {
            obs_data_release(settings);
            obs_properties_destroy(properties);
            return false;
        }

        auto current = obs_data_get_int(settings, key);
        normalizedValue = (double(current) - double(intMin)) / double(intMax - intMin);
    }
    else if (type == OBS_PROPERTY_FLOAT)
    {
        auto floatMin = obs_property_float_min(property);
        auto floatMax = obs_property_float_max(property);
        if (floatMax <= floatMin)
        {
            obs_data_release(settings);
            obs_properties_destroy(properties);
            return false;
        }

        auto current = obs_data_get_double(settings, key);
        normalizedValue = (current - floatMin) / (floatMax - floatMin);
    }
    else if (type == OBS_PROPERTY_LIST)
    {
        auto itemCount = obs_property_list_item_count(property);
        if (itemCount <= 0)
        {
            obs_data_release(settings);
            obs_properties_destroy(properties);
            return false;
        }

        auto selectedIndex = 0;
        auto format = obs_property_list_format(property);
        if (format == OBS_COMBO_FORMAT_INT)
        {
            auto selectedInt = obs_data_get_int(settings, key);
            for (size_t i = 0; i < itemCount; ++i)
                if (obs_property_list_item_int(property, i) == selectedInt)
                    selectedIndex = int(i);
        }
        else if (format == OBS_COMBO_FORMAT_FLOAT)
        {
            auto selectedFloat = obs_data_get_double(settings, key);
            for (size_t i = 0; i < itemCount; ++i)
                if (std::abs(obs_property_list_item_float(property, i) - selectedFloat) <= 1.0e-9)
                    selectedIndex = int(i);
        }
        else
        {
            auto selectedString = juce::String(obs_data_get_string(settings, key));
            for (size_t i = 0; i < itemCount; ++i)
                if (selectedString == juce::String(obs_property_list_item_string(property, i)))
                    selectedIndex = int(i);
        }

        normalizedValue = itemCount <= 1 ? 0.0 : double(selectedIndex) / double(itemCount - 1);
    }
    else
    {
        obs_data_release(settings);
        obs_properties_destroy(properties);
        return false;
    }

    normalizedValue = juce::jlimit(0.0, 1.0, normalizedValue);
    obs_data_release(settings);
    obs_properties_destroy(properties);
    return true;
}

bool writeFilterPropertyNormalizedValue(obs_source_t* filter, juce::String parameterKey, double normalizedValue)
{
    if (filter == nullptr || parameterKey.isEmpty())
        return false;

    auto* properties = obs_source_properties(filter);
    if (properties == nullptr)
        return false;

    auto* settings = obs_source_get_settings(filter);
    if (settings == nullptr)
    {
        obs_properties_destroy(properties);
        return false;
    }

    auto* property = obs_properties_get(properties, parameterKey.toRawUTF8());
    if (property == nullptr || !isWritableFilterProperty(property))
    {
        obs_data_release(settings);
        obs_properties_destroy(properties);
        return false;
    }

    auto key = obs_property_name(property);
    auto type = obs_property_get_type(property);
    auto clamped = juce::jlimit(0.0, 1.0, normalizedValue);

    if (type == OBS_PROPERTY_BOOL)
    {
        obs_data_set_bool(settings, key, clamped >= 0.5);
    }
    else if (type == OBS_PROPERTY_INT)
    {
        auto intMin = obs_property_int_min(property);
        auto intMax = obs_property_int_max(property);
        if (intMax <= intMin)
        {
            obs_data_release(settings);
            obs_properties_destroy(properties);
            return false;
        }

        auto value = int(std::lround(double(intMin) + clamped * double(intMax - intMin)));
        value = juce::jlimit(int(intMin), int(intMax), value);
        obs_data_set_int(settings, key, value);
    }
    else if (type == OBS_PROPERTY_FLOAT)
    {
        auto floatMin = obs_property_float_min(property);
        auto floatMax = obs_property_float_max(property);
        if (floatMax <= floatMin)
        {
            obs_data_release(settings);
            obs_properties_destroy(properties);
            return false;
        }

        auto value = floatMin + clamped * (floatMax - floatMin);
        obs_data_set_double(settings, key, value);
    }
    else if (type == OBS_PROPERTY_LIST)
    {
        auto itemCount = obs_property_list_item_count(property);
        if (itemCount <= 0)
        {
            obs_data_release(settings);
            obs_properties_destroy(properties);
            return false;
        }

        auto maxIndex = int(itemCount) - 1;
        auto selectedIndex = maxIndex <= 0 ? 0 : int(std::lround(clamped * double(maxIndex)));
        selectedIndex = juce::jlimit(0, maxIndex, selectedIndex);

        auto format = obs_property_list_format(property);
        if (format == OBS_COMBO_FORMAT_INT)
            obs_data_set_int(settings, key, obs_property_list_item_int(property, size_t(selectedIndex)));
        else if (format == OBS_COMBO_FORMAT_FLOAT)
            obs_data_set_double(settings, key, obs_property_list_item_float(property, size_t(selectedIndex)));
        else
            obs_data_set_string(settings, key, obs_property_list_item_string(property, size_t(selectedIndex)));
    }
    else
    {
        obs_data_release(settings);
        obs_properties_destroy(properties);
        return false;
    }

    obs_source_update(filter, settings);
    obs_source_update_properties(filter);

    obs_data_release(settings);
    obs_properties_destroy(properties);
    return true;
}

double toObservedValueFromMidiNormalized(const MidiControlMapping& mapping, double midiNormalizedValue)
{
    auto normalizedValue = juce::jlimit(0.0, 1.0, midiNormalizedValue);
    normalizedValue = juce::jlimit(0.0, 1.0, normalizedValue * mapping.scale);

    if (mapping.valueInverted)
        normalizedValue = 1.0 - normalizedValue;

    return mapping.minValue + ((mapping.maxValue - mapping.minValue) * normalizedValue);
}

double toMidiNormalizedFromObservedValue(const MidiControlMapping& mapping, double observedValue)
{
    auto range = mapping.maxValue - mapping.minValue;
    if (range <= 0.0)
        return mapping.valueInverted ? 1.0 : 0.0;

    if (mapping.scale <= 0.0)
        return 0.0;

    auto scaledValue = juce::jlimit(mapping.minValue, mapping.maxValue, observedValue);

    auto normalizedValue = (scaledValue - mapping.minValue) / range;
    normalizedValue = juce::jlimit(0.0, 1.0, normalizedValue);

    if (mapping.valueInverted)
        normalizedValue = 1.0 - normalizedValue;

    return juce::jlimit(0.0, 1.0, normalizedValue / mapping.scale);
}

int toPickupStep(const MidiControlMapping& mapping, double sourceDeflection)
{
    auto normalizedValue = toMidiNormalizedFromObservedValue(mapping, sourceDeflection);
    return juce::jlimit(0, 127, int(std::lround(normalizedValue * 127.0)));
}

bool shouldDisarmPickup(int currentSourceStep, int currentControlStep)
{
    return std::abs(currentSourceStep - currentControlStep) <= 1;
}

bool isIncrementDecrementMode(int interactionMode)
{
    return interactionMode == midi_control_interaction_mode_inc_dec
        || interactionMode == midi_control_interaction_mode_inc_dec2;
}

int getMidiValueFromMessage(const juce::MidiMessage& message)
{
    if (message.isController())
        return message.getControllerValue();

    if (message.isNoteOn())
        return juce::jlimit(0, 127, int(std::lround(message.getVelocity() * 127.0f)));

    if (message.isNoteOff())
        return 0;

    if (message.isProgramChange())
        return message.getProgramChangeNumber();

    failFast("MidiControlController::getMidiValueFromMessage", "unsupported MIDI message for incremental mapping");
}

int getRelativeStepDelta(int interactionMode, int midiValue)
{
    auto clampedValue = juce::jlimit(0, 127, midiValue);

    if (interactionMode == midi_control_interaction_mode_inc_dec2)
        return clampedValue - 63;

    if (clampedValue == 63 || clampedValue == 0)
        return 0;

    if (clampedValue < 63)
        return clampedValue;

    return -(128 - clampedValue);
}

bool matchesMappingInput(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    if (mapping.inputDeviceName.isNotEmpty() && mapping.inputDeviceName != event.inputDeviceName)
        return false;

    if (mapping.channel > 0 && mapping.channel != event.message.getChannel())
        return false;

    if (mapping.messageType == midi_control_message_type_cc)
        return event.message.isController() && event.message.getControllerNumber() == mapping.data1;

    if (mapping.messageType == midi_control_message_type_note)
        return (event.message.isNoteOn() || event.message.isNoteOff())
            && event.message.getNoteNumber() == mapping.data1;

    if (mapping.messageType == midi_control_message_type_program_change)
        return event.message.isProgramChange() && event.message.getProgramChangeNumber() == mapping.data1;

    failFast(
        "MidiControlController::processEvent",
        "unsupported messageType " + juce::String(mapping.messageType) + " for mapping " + mapping.mappingId
    );
}

bool isSupportedFeedbackTriggerMessage(const juce::MidiMessage& message)
{
    return message.isController() || message.isNoteOn() || message.isNoteOff() || message.isProgramChange();
}

void setSourceMonitoringEnabled(obs_source_t* source, bool enabled)
{
    obs_source_set_monitoring_type(source, enabled ? OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT : OBS_MONITORING_TYPE_NONE);
}

QObject* findObsTbarObject(QObject* root)
{
    if (root == nullptr)
        return nullptr;

    if (auto* slider = qobject_cast<QAbstractSlider*>(root))
    {
        if (slider->minimum() == 0 && slider->maximum() >= tbarMaxPosition)
            return slider;
    }

    const QObjectList& children = root->children();
    for (auto* child : children)
        if (auto* result = findObsTbarObject(child))
            return result;

    return nullptr;
}

bool setObsTbarSliderPosition(int position)
{
    auto* mainWindow = static_cast<QObject*>(obs_frontend_get_main_window());
    if (mainWindow == nullptr)
        return false;

    QObject* tbarObject = mainWindow->findChild<QObject*>(tbarObjectName);
    if (tbarObject == nullptr)
        tbarObject = findObsTbarObject(mainWindow);

    if (tbarObject == nullptr)
        return false;

    auto* slider = qobject_cast<QAbstractSlider*>(tbarObject);
    if (slider == nullptr)
        return false;

    if (QThread::currentThread() != slider->thread())
    {
        atk::logging::error(
            "MidiControlController::TBarEvent",
            "Fatal: TBar synchronization attempted from non-UI thread"
        );
        return false;
    }

    slider->setValue(position);
    return slider->value() == position;
}

void activateSceneSource(obs_source_t* sceneSource)
{
    if (sceneSource != nullptr)
    {
        if (obs_frontend_preview_program_mode_active())
            obs_frontend_set_current_preview_scene(sceneSource);
        else
            obs_frontend_set_current_scene(sceneSource);
    }
}

void activateAdjacentScene(bool next)
{
    obs_frontend_source_list sceneList = {};
    obs_frontend_get_scenes(&sceneList);

    if (sceneList.sources.num == 0)
    {
        obs_frontend_source_list_free(&sceneList);
        return;
    }

    obs_source_t* currentScene = nullptr;
    if (obs_frontend_preview_program_mode_active())
        currentScene = obs_frontend_get_current_preview_scene();

    if (currentScene == nullptr)
        currentScene = obs_frontend_get_current_scene();

    juce::String currentSceneUuid =
        currentScene != nullptr ? juce::String(obs_source_get_uuid(currentScene)) : juce::String();
    int currentIndex = -1;

    for (size_t i = 0; i < sceneList.sources.num; ++i)
    {
        if (currentSceneUuid == obs_source_get_uuid(sceneList.sources.array[i]))
        {
            currentIndex = int(i);
            break;
        }
    }

    if (currentIndex >= 0)
    {
        int step = next ? 1 : -1;
        int targetIndex = (currentIndex + step + int(sceneList.sources.num)) % int(sceneList.sources.num);
        activateSceneSource(sceneList.sources.array[targetIndex]);
    }

    if (currentScene != nullptr)
        obs_source_release(currentScene);

    obs_frontend_source_list_free(&sceneList);
}

void onFrontendEvent(enum ::obs_frontend_event event, void* privateData)
{
    auto* self = static_cast<MidiControlController*>(privateData);
    if (self == nullptr)
        return;

    if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
    {
        setLastTouchedPersistenceSuspended(true);
        self->stopRuntimeResources();
        return;
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING)
    {
        setLastTouchedPersistenceSuspended(true);
        return;
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP)
    {
        self->stopRuntimeResources();
        return;
    }

    if (event == OBS_FRONTEND_EVENT_TBAR_VALUE_CHANGED)
        self->handleTbarValueChanged();

    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
    {
        auto collectionId = getCurrentSceneCollectionId();
        setLastTouchedActiveCollection(collectionId);
        setLastTouchedPersistenceSuspended(false);
        ObsFilterLastTouchedTracker::getInstance().markParameterKeyCacheDirty();
        self->resumeRuntimeResources();
        self->restoreStateIfReady();
        self->requestFeedbackRefresh();

        MidiControlDialog::refreshIfOpen();
        MidiControlLastTouchedHelperDialog::refreshIfOpen();
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED
        || event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED
        || event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED
        || event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED)
    {
        self->requestFeedbackRefresh();

        MidiControlDialog::refreshIfOpen();
        MidiControlLastTouchedHelperDialog::refreshIfOpen();
    }

    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
    {
        auto collectionId = getCurrentSceneCollectionId();
        setLastTouchedActiveCollection(collectionId);
        setLastTouchedPersistenceSuspended(false);
        ObsFilterLastTouchedTracker::getInstance().markParameterKeyCacheDirty();
        self->restoreStateIfReady();

        if (MidiControlActiveContextDock::shouldRestoreOnStartup())
            MidiControlActiveContextDock::ensureRegistered();

        if (MidiControlLastTouchedHelperDialog::shouldRestoreOnStartup())
            MidiControlLastTouchedHelperDialog::ensureRegistered();

        MidiControlDialog::refreshIfOpen();
        MidiControlLastTouchedHelperDialog::refreshIfOpen();
    }
}

bool requiresObsFilterPoll(const std::vector<MidiControlMapping>& mappings)
{
    for (auto& mapping : mappings)
    {
        if (!mapping.enabled || mapping.outputMode == midi_control_output_mode_none)
            continue;

        if (mapping.actionType == midi_control_action_type_effect_parm
            && mapping.targetLayerToken.startsWithIgnoreCase(effectParmObsFilterLayerPrefix.data()))
            return true;

        if (mapping.actionType == midi_control_action_type_last_touched)
        {
            int offset = -1;
            if (parseObsFilterAudioLastTouchedTargetOffset(mapping.targetUuid, offset)
                || parseObsFilterVideoLastTouchedTargetOffset(mapping.targetUuid, offset))
                return true;
        }
    }

    return false;
}
} // namespace

MidiControlController* MidiControlController::getInstance()
{
    if (g_midiObsController == nullptr)
        g_midiObsController = new MidiControlController();

    return g_midiObsController;
}

MidiControlController* MidiControlController::getInstanceWithoutCreating()
{
    return g_midiObsController;
}

MidiControlController::MidiControlController()
    : midiClient(65536)
{
}

MidiControlController::~MidiControlController()
{
    shutdown();
    g_midiObsController = nullptr;
}

void MidiControlController::initialize()
{
    if (initialized)
        return;

    runtimeStopping = false;

    mappings = atk::settings::getMidiControlMappings();
    obsFilterPollEnabled = requiresObsFilterPoll(mappings);
    ObsFilterLastTouchedTracker::getInstance().markParameterKeyCacheDirty();
    subscriptions = atk::settings::getMidiControlSubscriptions();
    parameterAutoSyncEnabled = atk::settings::isMidiControlParameterAutoSyncEnabled();
    delayedFeedbackOutputEnabled = atk::settings::isMidiControlDelayedFeedbackOutputEnabled();
    matchByNameEnabled = atk::settings::isMidiControlMatchByNameEnabled();
    lastTouchedTrackingEnabled = atk::settings::isMidiControlLastTouchedTrackingEnabled();
    LastTouchedParameterTracker::getInstance().setTrackingEnabled(lastTouchedTrackingEnabled);
    delayedFeedbackOutputIdleMs =
        clampDelayedFeedbackOutputIdleMs(atk::settings::getMidiControlDelayedFeedbackOutputIdleMs());
    midiClient.setSubscriptions(subscriptions);
    volumeFader = obs_fader_create(OBS_FADER_LOG);
    feedbackVolumeFader = obs_fader_create(OBS_FADER_LOG);

    if (volumeFader == nullptr)
        failFast("MidiControlController::initialize", "failed to create OBS log volume fader");

    if (feedbackVolumeFader == nullptr)
        failFast("MidiControlController::initialize", "failed to create OBS log volume fader for feedback");

    obs_frontend_add_event_callback(onFrontendEvent, this);
    setLastTouchedActiveCollection(getCurrentSceneCollectionId());
    setLastTouchedPersistenceSuspended(false);
    pluginLastTouchedListenerId = LastTouchedParameterTracker::getInstance().addTouchListener(
        [this]() { pluginLastTouchedFeedbackPending.store(true, std::memory_order_release); }
    );
    obsFilterLastTouchedListenerId = ObsFilterLastTouchedTracker::getInstance().addTouchListener(
        [this]() { obsFilterLastTouchedFeedbackPending.store(true, std::memory_order_release); }
    );
    pluginLastTouchedFeedbackPending.store(false, std::memory_order_release);
    obsFilterLastTouchedFeedbackPending.store(false, std::memory_order_release);
    initialStateRestorePending = true;
    restoreStateIfReady();

    if (MidiControlActiveContextDock::shouldRestoreOnStartup())
        MidiControlActiveContextDock::ensureRegistered();

    if (MidiControlLastTouchedHelperDialog::shouldRestoreOnStartup())
        MidiControlLastTouchedHelperDialog::ensureRegistered();

    startTimerHz(60);
    initialized = true;
}

void MidiControlController::shutdown()
{
    if (!initialized)
        return;

    setLastTouchedPersistenceSuspended(true);
    stopRuntimeResources();

    if (pluginLastTouchedListenerId > 0)
    {
        LastTouchedParameterTracker::getInstance().removeTouchListener(pluginLastTouchedListenerId);
        pluginLastTouchedListenerId = 0;
    }

    if (obsFilterLastTouchedListenerId > 0)
    {
        ObsFilterLastTouchedTracker::getInstance().removeTouchListener(obsFilterLastTouchedListenerId);
        obsFilterLastTouchedListenerId = 0;
    }

    MidiControlActiveContextDock::unregisterDock();
    MidiControlLastTouchedHelperDialog::unregisterDock();

    obs_frontend_remove_event_callback(onFrontendEvent, this);

    initialized = false;
}

void MidiControlController::stopRuntimeResources()
{
    if (!initialized || runtimeStopping)
        return;

    runtimeStopping = true;

    stopTimer();

    midiClient.setSubscriptions(MidiClientState{});
    subscriptions = MidiClientState{};

    ObsFilterLastTouchedTracker::getInstance().suspend();
    clearSourceObservers();
    destroyFaders();
    resetRuntimeState();
}

void MidiControlController::resumeRuntimeResources()
{
    if (!initialized || !runtimeStopping)
        return;

    runtimeStopping = false;

    subscriptions = atk::settings::getMidiControlSubscriptions();
    midiClient.setSubscriptions(subscriptions);

    volumeFader = obs_fader_create(OBS_FADER_LOG);
    feedbackVolumeFader = obs_fader_create(OBS_FADER_LOG);

    if (volumeFader == nullptr)
        failFast("MidiControlController::resumeRuntimeResources", "failed to create OBS log volume fader");

    if (feedbackVolumeFader == nullptr)
        failFast("MidiControlController::resumeRuntimeResources", "failed to create OBS log volume fader for feedback");

    rebuildSourceObservers();
    ObsFilterLastTouchedTracker::getInstance().markParameterKeyCacheDirty();
    enqueueAllMappingsForFeedback();
    startTimerHz(60);
}

bool MidiControlController::isRuntimeActive() const
{
    return initialized && !runtimeStopping;
}

void MidiControlController::resetRuntimeState()
{
    toggleStates.clear();
    pickupStates.clear();
    transitionStates.clear();
    feedbackStates.clear();
    pendingFeedbackMappingIds.clear();
    delayedFeedbackByMappingId.clear();
    learnState = MidiControlLearnState{};
    feedbackRefreshRequested.store(false, std::memory_order_release);
    pluginLastTouchedFeedbackPending.store(false, std::memory_order_release);
    obsFilterLastTouchedFeedbackPending.store(false, std::memory_order_release);
    feedbackSyncPending = false;
    feedbackSyncDeadlineMs = 0.0;
    delayedFeedbackOutputIdleDeadlineMs = 0.0;
    sourceObserverRebuildPending = false;
    activeBank = mappingActiveContextMin;
    activePreset = mappingActiveContextMin;
    initialStateRestorePending = false;
}

void MidiControlController::destroyFaders()
{
    if (volumeFader != nullptr)
    {
        obs_fader_destroy(volumeFader);
        volumeFader = nullptr;
    }

    if (feedbackVolumeFader != nullptr)
    {
        obs_fader_destroy(feedbackVolumeFader);
        feedbackVolumeFader = nullptr;
    }
}

std::vector<MidiControlMapping> MidiControlController::getMappings() const
{
    return mappings;
}

bool MidiControlController::isParameterAutoSyncEnabled() const
{
    return parameterAutoSyncEnabled;
}

bool MidiControlController::isDelayedFeedbackOutputEnabled() const
{
    return delayedFeedbackOutputEnabled;
}

bool MidiControlController::isMatchByNameEnabled() const
{
    return matchByNameEnabled;
}

bool MidiControlController::isLastTouchedTrackingEnabled() const
{
    return lastTouchedTrackingEnabled;
}

int MidiControlController::getDelayedFeedbackOutputIdleMs() const
{
    return delayedFeedbackOutputIdleMs;
}

int MidiControlController::getActiveBank() const
{
    return activeBank;
}

int MidiControlController::getActivePreset() const
{
    return activePreset;
}

void MidiControlController::setActiveBank(int bank)
{
    auto nextBank = wrapMappingContextValue(bank);
    if (nextBank == activeBank)
        return;

    activeBank = nextBank;
    applyMappingContextChange();
    notifyActiveContextChanged(activeBank, activePreset);

    MidiControlActiveContextDock::refreshIfOpen();
    MidiControlLastTouchedHelperDialog::refreshIfOpen();
}

void MidiControlController::setActivePreset(int preset)
{
    auto nextPreset = wrapMappingContextValue(preset);
    if (nextPreset == activePreset)
        return;

    activePreset = nextPreset;
    applyMappingContextChange();
    notifyActiveContextChanged(activeBank, activePreset);

    MidiControlActiveContextDock::refreshIfOpen();
    MidiControlLastTouchedHelperDialog::refreshIfOpen();
}

void MidiControlController::setParameterAutoSyncEnabled(bool enabled)
{
    parameterAutoSyncEnabled = enabled;

    if (!parameterAutoSyncEnabled)
    {
        feedbackSyncPending = false;
        feedbackSyncDeadlineMs = 0.0;
    }

    atk::settings::setMidiControlParameterAutoSyncEnabled(enabled);
}

void MidiControlController::setDelayedFeedbackOutputEnabled(bool enabled)
{
    delayedFeedbackOutputEnabled = enabled;
    clearDelayedFeedbackQueue();
    atk::settings::setMidiControlDelayedFeedbackOutputEnabled(enabled);
}

void MidiControlController::setMatchByNameEnabled(bool enabled)
{
    if (matchByNameEnabled == enabled)
        return;

    matchByNameEnabled = enabled;

    if (isRuntimeActive() && !initialStateRestorePending)
    {
        sourceObserverRebuildPending = true;
        enqueueAllMappingsForFeedback();
    }

    atk::settings::setMidiControlMatchByNameEnabled(enabled);
}

void MidiControlController::setLastTouchedTrackingEnabled(bool enabled)
{
    if (lastTouchedTrackingEnabled == enabled)
        return;

    lastTouchedTrackingEnabled = enabled;
    LastTouchedParameterTracker::getInstance().setTrackingEnabled(enabled);
    atk::settings::setMidiControlLastTouchedTrackingEnabled(enabled);
}

void MidiControlController::setDelayedFeedbackOutputIdleMs(int delayMs)
{
    delayedFeedbackOutputIdleMs = clampDelayedFeedbackOutputIdleMs(delayMs);

    if (feedbackSyncPending)
        feedbackSyncDeadlineMs = juce::Time::getMillisecondCounterHiRes() + double(delayedFeedbackOutputIdleMs);

    clearDelayedFeedbackQueue();
    atk::settings::setMidiControlDelayedFeedbackOutputIdleMs(delayedFeedbackOutputIdleMs);
}

void MidiControlController::setMappings(const std::vector<MidiControlMapping>& newMappings)
{
    juce::String validationError;
    if (!validateMidiControlMappings(newMappings, validationError))
        failFast("MidiControlController::setMappings", validationError);

    mappings = newMappings;
    obsFilterPollEnabled = requiresObsFilterPoll(mappings);
    ObsFilterLastTouchedTracker::getInstance().markParameterKeyCacheDirty();
    pickupStates.clear();
    transitionStates.clear();
    feedbackStates.clear();
    pendingFeedbackMappingIds.clear();
    feedbackSyncPending = false;
    feedbackSyncDeadlineMs = 0.0;
    clearDelayedFeedbackQueue();

    if (!isRuntimeActive() || initialStateRestorePending)
    {
        atk::settings::setMidiControlMappings(newMappings);
        return;
    }

    rebuildSourceObservers();
    enqueueAllMappingsForFeedback();

    atk::settings::setMidiControlMappings(newMappings);
}

MidiClientState MidiControlController::getSubscriptions() const
{
    return subscriptions;
}

void MidiControlController::setSubscriptions(const MidiClientState& state)
{
    subscriptions = state;

    if (!isRuntimeActive())
    {
        atk::settings::setMidiControlSubscriptions(state);
        return;
    }

    midiClient.setSubscriptions(state);
    enqueueAllMappingsForFeedback();
    feedbackSyncPending = false;
    feedbackSyncDeadlineMs = 0.0;
    clearDelayedFeedbackQueue();
    atk::settings::setMidiControlSubscriptions(state);
}

juce::StringArray MidiControlController::getAvailableInputDevices() const
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        return server->getAvailableMidiInputDevices();

    return {};
}

juce::StringArray MidiControlController::getAvailableOutputDevices() const
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        return server->getAvailableMidiOutputDevices();

    return {};
}

void MidiControlController::beginLearning(const juce::String& mappingId)
{
    learnState.active = true;
    learnState.mappingId = mappingId;
    notifyLearnStateChanged(learnState);
}

void MidiControlController::cancelLearning()
{
    learnState.active = false;
    learnState.mappingId.clear();
    notifyLearnStateChanged(learnState);
}

MidiControlLearnState MidiControlController::getLearnState() const
{
    return learnState;
}

int MidiControlController::addLearnStateListener(std::function<void(const MidiControlLearnState&)> listener)
{
    if (!listener)
        return 0;

    const juce::ScopedLock lock(learnStateListenersLock);
    auto listenerId = nextLearnStateListenerId++;
    learnStateListeners.push_back({listenerId, std::move(listener)});
    return listenerId;
}

void MidiControlController::removeLearnStateListener(int listenerId)
{
    if (listenerId <= 0)
        return;

    const juce::ScopedLock lock(learnStateListenersLock);

    learnStateListeners.erase(
        std::remove_if(
            learnStateListeners.begin(),
            learnStateListeners.end(),
            [listenerId](const std::pair<int, std::function<void(const MidiControlLearnState&)>>& entry)
            { return entry.first == listenerId; }
        ),
        learnStateListeners.end()
    );
}

int MidiControlController::addActiveContextListener(std::function<void(int, int)> listener)
{
    if (!listener)
        return 0;

    const juce::ScopedLock lock(activeContextListenersLock);
    auto listenerId = nextActiveContextListenerId++;
    activeContextListeners.push_back({listenerId, std::move(listener)});
    return listenerId;
}

void MidiControlController::removeActiveContextListener(int listenerId)
{
    if (listenerId <= 0)
        return;

    const juce::ScopedLock lock(activeContextListenersLock);

    activeContextListeners.erase(
        std::remove_if(
            activeContextListeners.begin(),
            activeContextListeners.end(),
            [listenerId](const std::pair<int, std::function<void(int, int)>>& entry)
            { return entry.first == listenerId; }
        ),
        activeContextListeners.end()
    );
}

void MidiControlController::notifyLearnStateChanged(const MidiControlLearnState& state)
{
    std::vector<std::function<void(const MidiControlLearnState&)>> listenersCopy;

    {
        const juce::ScopedLock lock(learnStateListenersLock);
        listenersCopy.reserve(learnStateListeners.size());
        for (const auto& entry : learnStateListeners)
            listenersCopy.push_back(entry.second);
    }

    for (auto& listener : listenersCopy)
        if (listener)
            listener(state);
}

void MidiControlController::notifyActiveContextChanged(int bank, int preset)
{
    std::vector<std::function<void(int, int)>> listenersCopy;

    {
        const juce::ScopedLock lock(activeContextListenersLock);
        listenersCopy.reserve(activeContextListeners.size());
        for (const auto& entry : activeContextListeners)
            listenersCopy.push_back(entry.second);
    }

    for (auto& listener : listenersCopy)
        if (listener)
            listener(bank, preset);
}

void MidiControlController::requestFeedbackRefresh()
{
    if (!isRuntimeActive())
        return;

    feedbackRefreshRequested.store(true, std::memory_order_release);
}

void MidiControlController::restoreStateIfReady()
{
    if (!isRuntimeActive())
        return;

    if (!initialStateRestorePending)
        return;

    auto* currentScene = obs_frontend_get_current_scene();
    if (currentScene == nullptr)
        return;

    obs_source_release(currentScene);

    juce::String validationError;
    if (!validateMidiControlMappings(mappings, validationError))
        failFast("MidiControlController::restoreStateIfReady", validationError);

    rebuildSourceObservers();
    enqueueAllMappingsForFeedback();
    initialStateRestorePending = false;
}

void MidiControlController::timerCallback()
{
    if (!isRuntimeActive())
        return;

    auto refreshLastTouchedDock = false;

    if (sourceObserverRebuildPending)
    {
        sourceObserverRebuildPending = false;
        rebuildSourceObservers();
    }

    if (obsFilterPollEnabled)
        ObsFilterLastTouchedTracker::getInstance().poll();

    if (pluginLastTouchedFeedbackPending.exchange(false, std::memory_order_acq_rel))
    {
        enqueueLastTouchedMappingsForFeedback();
        refreshLastTouchedDock = true;
    }

    if (obsFilterLastTouchedFeedbackPending.exchange(false, std::memory_order_acq_rel))
    {
        enqueueLastTouchedMappingsForFeedback();
        enqueueEffectParmMappingsForFeedback();
        refreshLastTouchedDock = true;
    }

    if (feedbackRefreshRequested.exchange(false, std::memory_order_acq_rel))
    {
        enqueueAllMappingsForFeedback();
        refreshLastTouchedDock = true;
    }

    if (refreshLastTouchedDock)
        MidiControlLastTouchedHelperDialog::refreshIfOpen();

    pendingMidiEventsScratch.clear();
    midiClient.getPendingMidiEvents(pendingMidiEventsScratch, 4096, 48000.0);

    for (auto& event : pendingMidiEventsScratch)
    {
        processEvent(event);
        requestFeedbackSyncFromInput(event);
    }

    processFeedbackSyncIfDue();
    processPendingFeedbackEvents();
    processDelayedFeedbackFlushIfDue();
}

void MidiControlController::requestFeedbackSyncFromInput(const MidiInputEvent& event)
{
    if (!parameterAutoSyncEnabled)
        return;

    if (!isSupportedFeedbackTriggerMessage(event.message))
        return;

    feedbackSyncPending = true;
    feedbackSyncDeadlineMs = juce::Time::getMillisecondCounterHiRes() + double(delayedFeedbackOutputIdleMs);
}

void MidiControlController::processFeedbackSyncIfDue()
{
    if (!feedbackSyncPending)
        return;

    auto nowMs = juce::Time::getMillisecondCounterHiRes();
    if (nowMs < feedbackSyncDeadlineMs)
        return;

    feedbackSyncPending = false;
    feedbackSyncDeadlineMs = 0.0;
    enqueueAllMappingsForFeedback();
    processPendingFeedbackEvents(true);

    if (delayedFeedbackOutputEnabled)
        flushDelayedFeedbackOutputNow();
}

void MidiControlController::processPendingFeedbackEvents(bool forceSend)
{
    if (pendingFeedbackMappingIds.empty())
        return;

    pendingFeedbackScratch.clear();
    pendingFeedbackScratch.swap(pendingFeedbackMappingIds);
    activeFeedbackMappingsScratch.clear();
    activeFeedbackMappingsScratch.reserve(mappings.size());

    for (auto& mapping : mappings)
    {
        if (pendingFeedbackScratch.find(mapping.mappingId.toStdString()) == pendingFeedbackScratch.end())
            continue;

        if (!isMappingEligibleForFeedbackInCurrentContext(mapping))
            continue;

        if (isFeedbackMappingActiveForMapping(mapping, matchByNameEnabled))
        {
            activeFeedbackMappingsScratch.push_back(&mapping);
            continue;
        }

        processFeedbackForMapping(mapping, forceSend);
    }

    for (auto* mapping : activeFeedbackMappingsScratch)
        if (mapping != nullptr)
            processFeedbackForMapping(*mapping, forceSend);

    pendingFeedbackScratch.clear();
}

void MidiControlController::processDelayedFeedbackFlushIfDue()
{
    if (!delayedFeedbackOutputEnabled)
        return;

    if (delayedFeedbackByMappingId.empty())
        return;

    auto nowMs = juce::Time::getMillisecondCounterHiRes();
    if (nowMs < delayedFeedbackOutputIdleDeadlineMs)
        return;

    flushDelayedFeedbackOutputNow();
}

void MidiControlController::flushDelayedFeedbackOutputNow()
{
    if (delayedFeedbackByMappingId.empty())
        return;

    for (auto& entry : delayedFeedbackByMappingId)
    {
        auto& pending = entry.second;
        sendFeedbackMessage(pending.mapping, pending.normalizedValue);

        auto& state = feedbackStates[entry.first];
        state.lastSentValue = pending.normalizedValue;
        state.hasLastSentValue = true;
        if (pending.inactiveClear)
            state.inactiveFeedbackCleared = true;
    }

    delayedFeedbackByMappingId.clear();
    delayedFeedbackOutputIdleDeadlineMs = 0.0;
}

void MidiControlController::processFeedbackForMapping(const MidiControlMapping& mapping, bool forceSend)
{
    if (!mapping.enabled)
        return;

    if (!isFeedbackAction(mapping.actionType))
        return;

    if (!isFeedbackRouteActive(mapping))
        return;

    auto key = mapping.mappingId.toStdString();
    auto& state = feedbackStates[key];

    auto sendInactiveClear = [&]()
    {
        if (!forceSend && state.inactiveFeedbackCleared)
            return;

        if (delayedFeedbackOutputEnabled)
            queueDelayedFeedback(mapping, 0.0, true);
        else
        {
            sendFeedbackMessage(mapping, 0.0);
            state.hasLastSentValue = true;
            state.lastSentValue = 0.0;
            state.inactiveFeedbackCleared = true;
        }
    };

    if (!isMappingWinnerForCurrentContext(mapping))
    {
        sendInactiveClear();
        return;
    }

    double observedValue = 0.0;

    if (mapping.actionType == midi_control_action_type_last_touched)
    {
        if (isLastTouchedHoldTargetToken(mapping.targetUuid))
        {
            state.inactiveFeedbackCleared = false;
            observedValue = LastTouchedParameterTracker::getInstance().isHistoryHoldEnabled() ? 1.0 : 0.0;
        }
        else
        {
            int offset = -1;
            if (parseLastTouchedTargetOffset(mapping.targetUuid, offset))
            {
                LastTouchedParameterEntry parameterEntry;
                if (!LastTouchedParameterTracker::getInstance().getParameterAtOffset(offset, parameterEntry)
                    || parameterEntry.parameter == nullptr
                    || !isLastTouchedEntryActiveForMapping(mapping, parameterEntry))
                {
                    sendInactiveClear();
                    return;
                }

                state.inactiveFeedbackCleared = false;
                observedValue = juce::jlimit(0.0, 1.0, double(parameterEntry.parameter->getValue()));
            }
            else if (parseObsFilterAudioLastTouchedTargetOffset(mapping.targetUuid, offset))
            {
                ObsFilterLastTouchedEntry parameterEntry;
                if (!ObsFilterLastTouchedTracker::getInstance().getAudioParameterAtOffset(offset, parameterEntry)
                    || !isObsFilterLastTouchedEntryActiveForMapping(mapping, parameterEntry))
                {
                    sendInactiveClear();
                    return;
                }

                state.inactiveFeedbackCleared = false;
                observedValue = juce::jlimit(0.0, 1.0, parameterEntry.normalizedValue);
            }
            else if (parseObsFilterVideoLastTouchedTargetOffset(mapping.targetUuid, offset))
            {
                ObsFilterLastTouchedEntry parameterEntry;
                if (!ObsFilterLastTouchedTracker::getInstance().getVideoParameterAtOffset(offset, parameterEntry)
                    || !isObsFilterLastTouchedEntryActiveForMapping(mapping, parameterEntry))
                {
                    sendInactiveClear();
                    return;
                }

                state.inactiveFeedbackCleared = false;
                observedValue = juce::jlimit(0.0, 1.0, parameterEntry.normalizedValue);
            }
            else
            {
                sendInactiveClear();
                return;
            }
        }
    }
    else if (mapping.actionType == midi_control_action_type_mapping_context)
    {
        int targetType = midi_control_mapping_context_target_type_none;
        if (!parseMidiControlMappingContextTargetType(mapping.targetUuid, targetType))
        {
            sendInactiveClear();
            return;
        }

        int directValue = 0;
        int relativeStep = 0;
        auto parsed =
            parseMidiControlMappingContextParameterToken(mapping.targetParameterToken, directValue, relativeStep);

        if (!parsed || directValue < mappingActiveContextMin || directValue > mappingActiveContextMax)
        {
            sendInactiveClear();
            return;
        }

        state.inactiveFeedbackCleared = false;
        auto activeValue = targetType == midi_control_mapping_context_target_type_bank ? activeBank : activePreset;
        observedValue = activeValue == directValue ? 1.0 : 0.0;
    }
    else
    {
        auto* source = findTargetSource(mapping);
        if (source == nullptr)
        {
            sendInactiveClear();
            return;
        }

        if (!isSourceActiveForMapping(mapping, source))
        {
            obs_source_release(source);
            sendInactiveClear();
            return;
        }

        state.inactiveFeedbackCleared = false;
        observedValue = getObservedActionValue(mapping, source);
        obs_source_release(source);
    }

    auto normalizedValue = toMidiNormalizedValue(mapping, observedValue);
    auto nowMs = juce::Time::getMillisecondCounterHiRes();

    if (!forceSend && nowMs < state.suppressUntilMs)
        return;

    if (mapping.actionType == midi_control_action_type_source && !forceSend && state.hasLastSentValue)
    {
        int sourceActionType = midi_control_source_action_type_none;
        if (parseMidiControlSourceActionType(mapping.targetLayerToken, sourceActionType)
            && (sourceActionType == midi_control_source_action_type_mute
                || sourceActionType == midi_control_source_action_type_monitoring))
        {
            bool current = normalizedValue >= 0.5;
            bool last = state.lastSentValue >= 0.5;
            if (current == last)
                return;
        }
    }
    if (mapping.actionType == midi_control_action_type_mapping_context && !forceSend && state.hasLastSentValue)
    {
        bool current = normalizedValue >= 0.5;
        bool last = state.lastSentValue >= 0.5;
        if (current == last)
            return;
    }

    if ((mapping.actionType == midi_control_action_type_source
         || mapping.actionType == midi_control_action_type_last_touched
         || mapping.actionType == midi_control_action_type_effect_parm)
        && !forceSend
        && state.hasLastSentValue)
        if (std::abs(normalizedValue - state.lastSentValue) < feedbackVolumeThreshold)
            return;

    if (delayedFeedbackOutputEnabled)
    {
        queueDelayedFeedback(mapping, normalizedValue, false);
        return;
    }

    sendFeedbackMessage(mapping, normalizedValue);

    state.lastSentValue = normalizedValue;
    state.hasLastSentValue = true;
}

void MidiControlController::handleTbarValueChanged()
{
    if (!obs_frontend_preview_program_mode_active())
        return;

    for (auto& stateEntry : transitionStates)
    {
        auto& transitionState = stateEntry.second;
        if (!transitionState.releasePending || transitionState.lastPosition < tbarCompleteThreshold)
            continue;

        transitionState.releasePending = false;

        if (!setObsTbarSliderPosition(tbarMaxPosition))
        {
            atk::logging::error(
                "MidiControlController::TBarEvent",
                "Fatal: failed to synchronize OBS TBar slider before release for mapping "
                    + juce::String(stateEntry.first)
            );
            std::abort();
        }

        obs_frontend_release_tbar();

        transitionState.lockedUntilZero = true;
        transitionState.lastPosition = tbarMaxPosition;
    }
}

void MidiControlController::processEvent(const MidiInputEvent& event)
{
    handleLearningEvent(event);

    dispatchMappingsScratch.clear();
    dispatchMappingsScratch.reserve(mappings.size());
    int maxSpecificity = -1;

    for (auto& mapping : mappings)
    {
        if (!mapping.enabled)
            continue;

        if (!matchesMappingInput(mapping, event))
            continue;

        auto specificity = getMappingContextSpecificity(mapping);
        if (specificity < 0)
            continue;

        if (specificity > maxSpecificity)
            maxSpecificity = specificity;

        dispatchMappingsScratch.push_back({&mapping, specificity});
    }

    for (auto& entry : dispatchMappingsScratch)
    {
        auto* mapping = entry.first;
        if (mapping == nullptr || entry.second != maxSpecificity)
            continue;

        dispatchMapping(*mapping, event);
    }
}

void MidiControlController::handleLearningEvent(const MidiInputEvent& event)
{
    if (!learnState.active)
        return;

    if (!(event.message.isController() || event.message.isNoteOn() || event.message.isProgramChange()))
        return;

    learnState.event = event;
    learnState.sequence += 1;
    learnState.active = false;
    notifyLearnStateChanged(learnState);
}

void MidiControlController::dispatchMapping(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    if (mapping.actionType == midi_control_action_type_none)
        return;

    if (mapping.actionType == midi_control_action_type_transition)
    {
        dispatchTransitionMapping(mapping, event);
        return;
    }

    if (mapping.actionType == midi_control_action_type_scene)
    {
        dispatchSceneMapping(mapping, event);
        return;
    }

    if (mapping.actionType == midi_control_action_type_hotkey)
    {
        dispatchHotkeyMapping(mapping, event);
        return;
    }

    if (mapping.actionType == midi_control_action_type_mapping_context)
    {
        dispatchMappingContextSwitch(mapping, event);
        return;
    }

    if (mapping.actionType == midi_control_action_type_last_touched)
    {
        dispatchLastTouchedMapping(mapping, event);
        return;
    }

    if (mapping.actionType == midi_control_action_type_effect_parm)
    {
        dispatchEffectParmMapping(mapping, event);
        return;
    }

    dispatchSourceMapping(mapping, event);
}

void MidiControlController::dispatchHotkeyMapping(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    if (!shouldActivateFromEvent(mapping, event.message))
        return;

    auto hotkeyId = resolveHotkeyIdFromMappingTarget(mapping);
    if (hotkeyId == OBS_INVALID_HOTKEY_ID)
        return;

    obs_hotkey_trigger_routed_callback(hotkeyId, true);
    obs_hotkey_trigger_routed_callback(hotkeyId, false);
}

void MidiControlController::dispatchMappingContextSwitch(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    int targetType = midi_control_mapping_context_target_type_none;
    if (!parseMidiControlMappingContextTargetType(mapping.targetUuid, targetType))
        return;

    int directValue = 0;
    int relativeStep = 0;
    auto parsed = parseMidiControlMappingContextParameterToken(mapping.targetParameterToken, directValue, relativeStep);

    if (!parsed)
        return;

    if (isIncrementDecrementMode(mapping.interactionMode))
    {
        auto stepDelta = getRelativeStepDelta(mapping.interactionMode, getMidiValueFromMessage(event.message));
        if (mapping.valueInverted)
            stepDelta = -stepDelta;

        if (stepDelta == 0)
            return;

        relativeStep = stepDelta > 0 ? 1 : -1;
        directValue = 0;
    }
    else if (!shouldActivateFromEvent(mapping, event.message))
    {
        return;
    }
    else if (mapping.valueInverted)
    {
        relativeStep = -relativeStep;
    }

    auto nextBank = activeBank;
    auto nextPreset = activePreset;

    if (targetType == midi_control_mapping_context_target_type_bank)
    {
        if (relativeStep != 0)
            nextBank = wrapMappingContextValue(activeBank + relativeStep);
        else if (directValue >= mappingActiveContextMin && directValue <= mappingActiveContextMax)
            nextBank = directValue;
    }
    else
    {
        if (relativeStep != 0)
            nextPreset = wrapMappingContextValue(activePreset + relativeStep);
        else if (directValue >= mappingActiveContextMin && directValue <= mappingActiveContextMax)
            nextPreset = directValue;
    }

    if (nextBank == activeBank && nextPreset == activePreset)
        return;

    activeBank = nextBank;
    activePreset = nextPreset;
    applyMappingContextChange();
    notifyActiveContextChanged(activeBank, activePreset);
}

void MidiControlController::dispatchTransitionMapping(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    auto transitionKey = mapping.mappingId.toStdString();
    auto& transitionState = transitionStates[transitionKey];

    if (!obs_frontend_preview_program_mode_active())
    {
        resetTransitionState(transitionState, true);
        return;
    }

    if (mapping.interactionMode == midi_control_interaction_mode_absolute)
    {
        dispatchTransitionAbsoluteMapping(mapping, event, transitionState);
        return;
    }

    resetTransitionState(transitionState, false);

    if (shouldActivateFromEvent(mapping, event.message))
        obs_frontend_preview_program_trigger_transition();
}

void MidiControlController::dispatchTransitionAbsoluteMapping(
    const MidiControlMapping& mapping,
    const MidiInputEvent& event,
    MidiControlTransitionState& transitionState
)
{
    auto mappedValue = getAbsoluteValue(mapping, event.message);
    auto normalizedValue = juce::jlimit(0.0, 1.0, mappedValue);
    auto tbarPosition = juce::jlimit(0, tbarMaxPosition, int(std::lround(normalizedValue * double(tbarMaxPosition))));

    auto atZero = tbarPosition <= tbarZeroThreshold;
    auto atComplete = tbarPosition >= tbarCompleteThreshold;
    auto previousPosition = transitionState.lastPosition;

    if (!atComplete)
        transitionState.releasePending = false;

    if (transitionState.lockedUntilZero)
    {
        if (atZero)
            resetTransitionState(transitionState, false);

        return;
    }

    if (atComplete)
    {
        transitionState.releasePending = true;
        transitionState.lastPosition = tbarMaxPosition;
        obs_frontend_set_tbar_position(tbarMaxPosition);
        return;
    }

    if (transitionState.lastPosition != tbarPosition)
    {
        obs_frontend_set_tbar_position(tbarPosition);
        transitionState.lastPosition = tbarPosition;
    }

    if (atZero)
    {
        if (previousPosition > tbarZeroThreshold || hasActiveTbarState(transitionState))
            obs_frontend_release_tbar();

        resetTransitionState(transitionState, false);
    }
}

void MidiControlController::resetTransitionState(MidiControlTransitionState& transitionState, bool releaseTbarIfActive)
{
    if (releaseTbarIfActive && hasActiveTbarState(transitionState))
        obs_frontend_release_tbar();

    transitionState = MidiControlTransitionState{};
}

bool MidiControlController::handleLastTouchedHoldTarget(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    if (!isLastTouchedHoldTargetToken(mapping.targetUuid))
        return false;

    auto& tracker = LastTouchedParameterTracker::getInstance();
    auto holdStateChanged = false;

    if (mapping.interactionMode == midi_control_interaction_mode_toggle)
    {
        if (!shouldActivateFromEvent(mapping, event.message))
            return true;

        tracker.toggleHistoryHoldEnabled();
        holdStateChanged = true;
    }
    else if (mapping.interactionMode == midi_control_interaction_mode_absolute)
    {
        auto holdEnabled = getAbsoluteSwitchValue(mapping, event.message);
        if (tracker.isHistoryHoldEnabled() != holdEnabled)
        {
            tracker.setHistoryHoldEnabled(holdEnabled);
            holdStateChanged = true;
        }
    }
    else if (isIncrementDecrementMode(mapping.interactionMode))
    {
        auto stepDelta = getRelativeStepDelta(mapping.interactionMode, getMidiValueFromMessage(event.message));
        if (mapping.valueInverted)
            stepDelta = -stepDelta;

        if (stepDelta == 0)
            return true;

        auto holdEnabled = stepDelta > 0;
        if (tracker.isHistoryHoldEnabled() != holdEnabled)
        {
            tracker.setHistoryHoldEnabled(holdEnabled);
            holdStateChanged = true;
        }
    }
    else
    {
        if (!shouldActivateFromEvent(mapping, event.message))
            return true;

        auto holdEnabled = !mapping.valueInverted;
        if (tracker.isHistoryHoldEnabled() != holdEnabled)
        {
            tracker.setHistoryHoldEnabled(holdEnabled);
            holdStateChanged = true;
        }
    }

    if (holdStateChanged)
        requestFeedbackRefresh();

    return true;
}

void MidiControlController::dispatchSceneMapping(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    if (!shouldActivateFromEvent(mapping, event.message))
        return;

    if (mapping.targetName == previousSceneTargetName)
        activateAdjacentScene(false);
    else if (mapping.targetName == nextSceneTargetName)
        activateAdjacentScene(true);
    else
    {
        auto* sceneSource = findTargetSource(mapping);
        if (isSceneSource(sceneSource))
            activateSceneSource(sceneSource);

        if (sceneSource != nullptr)
            obs_source_release(sceneSource);
    }
}

void MidiControlController::dispatchSourceMapping(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    int sourceActionType = midi_control_source_action_type_none;
    if (!parseMidiControlSourceActionType(mapping.targetLayerToken, sourceActionType))
        return;

    auto* source = findTargetSource(mapping);
    if (source == nullptr)
        return;

    if (!isSourceActiveForMapping(mapping, source))
    {
        obs_source_release(source);
        return;
    }

    if (sourceActionType == midi_control_source_action_type_volume)
        handleSourceVolumeMapping(mapping, event, source);
    else
        handleSourceDiscreteMapping(mapping, event, source);

    obs_source_release(source);
}

void MidiControlController::dispatchLastTouchedMapping(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    if (handleLastTouchedHoldTarget(mapping, event))
        return;

    auto& lastTouchedTracker = LastTouchedParameterTracker::getInstance();

    int offset = -1;
    if (parseLastTouchedTargetOffset(mapping.targetUuid, offset))
    {
        LastTouchedParameterEntry parameterEntry;
        if (!LastTouchedParameterTracker::getInstance().getParameterAtOffset(offset, parameterEntry))
            return;

        if (!isLastTouchedEntryActiveForMapping(mapping, parameterEntry))
            return;

        auto* parameter = parameterEntry.parameter;
        if (parameter == nullptr)
            return;

        auto currentValue = juce::jlimit(0.0, 1.0, double(parameter->getValue()));
        auto nextValue = currentValue;

        if (isIncrementDecrementMode(mapping.interactionMode))
        {
            auto stepDelta = getRelativeStepDelta(mapping.interactionMode, getMidiValueFromMessage(event.message));

            if (mapping.valueInverted)
                stepDelta = -stepDelta;

            if (stepDelta == 0)
                return;

            auto range = mapping.maxValue - mapping.minValue;
            if (range <= 0.0)
                return;

            auto stepSize = (double(stepDelta) / 127.0) * mapping.scale * range;
            nextValue = currentValue + stepSize;
            nextValue = juce::jlimit(mapping.minValue, mapping.maxValue, nextValue);
        }
        else if (mapping.interactionMode == midi_control_interaction_mode_absolute)
        {
            auto absoluteValue = getAbsoluteValue(mapping, event.message);

            auto pickupKey = mapping.mappingId.toStdString();
            pickupKey += "::" + parameterEntry.ownerSourceUuid.toStdString();
            pickupKey +=
                "::" + juce::String::toHexString((juce::pointer_sized_int)parameterEntry.processor).toStdString();
            pickupKey += "::" + juce::String(parameterEntry.parameterIndex).toStdString();

            auto currentTargetIdentity = juce::String(pickupKey);

            auto currentParameterStep = toPickupStep(mapping, currentValue);
            auto currentControlStep = toPickupStep(mapping, absoluteValue);

            auto& pickupState = pickupStates[pickupKey];

            if (pickupState.targetUuid != currentTargetIdentity)
            {
                pickupState.targetUuid = currentTargetIdentity;
                pickupState.armed = true;
                pickupState.lastSourceStep = -1;
                pickupState.lastTouchSequence = parameterEntry.touchSequence;
            }

            if (pickupState.lastTouchSequence != parameterEntry.touchSequence)
            {
                pickupState.armed = true;
                pickupState.lastSourceStep = -1;
                pickupState.lastTouchSequence = parameterEntry.touchSequence;
            }

            if (pickupState.lastSourceStep >= 0 && currentParameterStep != pickupState.lastSourceStep)
                pickupState.armed = true;

            if (pickupState.armed)
            {
                if (shouldDisarmPickup(currentParameterStep, currentControlStep))
                    pickupState.armed = false;
                else
                    return;
            }

            pickupState.lastSourceStep = currentParameterStep;
            nextValue = absoluteValue;
            pickupState.lastSourceStep = currentControlStep;
        }
        else
        {
            if (!shouldActivateFromEvent(mapping, event.message))
                return;

            if (mapping.interactionMode == midi_control_interaction_mode_toggle)
            {
                auto key = mapping.mappingId.toStdString();
                auto stateValue = !toggleStates[key];
                toggleStates[key] = stateValue;
                nextValue = stateValue ? 1.0 : 0.0;
            }
            else
            {
                nextValue = 1.0;
            }

            if (mapping.valueInverted)
                nextValue = 1.0 - nextValue;
        }

        nextValue = juce::jlimit(0.0, 1.0, nextValue);
        if (std::abs(nextValue - currentValue) < 1.0e-6)
            return;

        lastTouchedTracker.beginInternalWrite(*parameter);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(float(nextValue));
        parameter->endChangeGesture();
        lastTouchedTracker.endInternalWrite(*parameter);
        return;
    }

    ObsFilterLastTouchedEntry parameterEntry;
    if (parseObsFilterAudioLastTouchedTargetOffset(mapping.targetUuid, offset))
    {
        if (!ObsFilterLastTouchedTracker::getInstance().getAudioParameterAtOffset(offset, parameterEntry))
            return;
    }
    else if (parseObsFilterVideoLastTouchedTargetOffset(mapping.targetUuid, offset))
    {
        if (!ObsFilterLastTouchedTracker::getInstance().getVideoParameterAtOffset(offset, parameterEntry))
            return;
    }
    else
    {
        return;
    }

    if (!isObsFilterLastTouchedEntryActiveForMapping(mapping, parameterEntry))
        return;

    auto currentValue = juce::jlimit(0.0, 1.0, parameterEntry.normalizedValue);
    auto nextValue = currentValue;

    if (isIncrementDecrementMode(mapping.interactionMode))
    {
        auto stepDelta = getRelativeStepDelta(mapping.interactionMode, getMidiValueFromMessage(event.message));

        if (mapping.valueInverted)
            stepDelta = -stepDelta;

        if (stepDelta == 0)
            return;

        auto range = mapping.maxValue - mapping.minValue;
        if (range <= 0.0)
            return;

        auto stepSize = (double(stepDelta) / 127.0) * mapping.scale * range;
        nextValue = currentValue + stepSize;
        nextValue = juce::jlimit(mapping.minValue, mapping.maxValue, nextValue);
    }
    else if (mapping.interactionMode == midi_control_interaction_mode_absolute)
    {
        nextValue = getAbsoluteValue(mapping, event.message);
    }
    else
    {
        if (!shouldActivateFromEvent(mapping, event.message))
            return;

        if (mapping.interactionMode == midi_control_interaction_mode_toggle)
        {
            auto key = mapping.mappingId.toStdString() + "::" + parameterEntry.identity.toStdString();
            auto stateValue = !toggleStates[key];
            toggleStates[key] = stateValue;
            nextValue = stateValue ? 1.0 : 0.0;
        }
        else
        {
            nextValue = 1.0;
        }

        if (mapping.valueInverted)
            nextValue = 1.0 - nextValue;
    }

    nextValue = juce::jlimit(0.0, 1.0, nextValue);
    if (std::abs(nextValue - currentValue) < 1.0e-6)
        return;

    if (!ObsFilterLastTouchedTracker::getInstance().setParameterNormalizedValue(parameterEntry, nextValue))
        logging::warning(
            "MidiControlController::dispatchLastTouchedMapping",
            "failed to write OBS filter parameter; sourceUuid="
                + parameterEntry.sourceUuid
                + " filterUuid="
                + parameterEntry.filterUuid
                + " parameter="
                + parameterEntry.parameterKey
                + " value="
                + juce::String(nextValue, 4)
        );
    else
        requestFeedbackRefresh();
}

void MidiControlController::dispatchEffectParmMapping(const MidiControlMapping& mapping, const MidiInputEvent& event)
{
    auto* source = findTargetSource(mapping);
    if (source == nullptr)
        return;

    if (!isSourceActiveForMapping(mapping, source))
    {
        obs_source_release(source);
        return;
    }

    EffectParmSelection selection;
    if (!parseEffectParmObsFilterSelection(mapping, selection))
    {
        obs_source_release(source);
        return;
    }

    auto resolvedLayerToken = mapping.targetLayerToken;
    auto* filter = findFilterByUuidOnSource(source, selection.filterUuid);

    if (filter == nullptr && matchByNameEnabled && mapping.targetLayerName.isNotEmpty())
    {
        filter = findFilterByNameOnSource(source, mapping.targetLayerName);
        if (filter != nullptr)
        {
            auto* resolvedFilterUuid = obs_source_get_uuid(filter);
            if (resolvedFilterUuid != nullptr && resolvedFilterUuid[0] != '\0')
            {
                selection.filterUuid = juce::String(resolvedFilterUuid);
                resolvedLayerToken = createEffectParmLayerToken(selection.filterUuid);
            }
        }
    }

    obs_source_release(source);
    if (filter == nullptr)
        return;

    if (resolvedLayerToken.isNotEmpty() && resolvedLayerToken != mapping.targetLayerToken)
        applyMappingAutoHeal(mapping, {}, resolvedLayerToken);

    auto pluginFilter = isPluginHostFilter(filter);

    double currentValue = 0.0;
    auto readCurrentValue = false;

    int pluginParameterIndex = -1;
    auto hasPluginParameterToken = parsePluginParameterIndexToken(selection.parameterKey, pluginParameterIndex);
    juce::ignoreUnused(pluginParameterIndex);

    if (hasPluginParameterToken && pluginFilter)
        readCurrentValue = readPluginParameterNormalizedValue(mapping, filter, selection.parameterKey, currentValue);
    else
        readCurrentValue = readFilterPropertyNormalizedValue(filter, selection.parameterKey, currentValue);

    if (!readCurrentValue)
    {
        obs_source_release(filter);
        return;
    }

    auto nextValue = currentValue;

    if (isIncrementDecrementMode(mapping.interactionMode))
    {
        auto stepDelta = getRelativeStepDelta(mapping.interactionMode, getMidiValueFromMessage(event.message));
        if (mapping.valueInverted)
            stepDelta = -stepDelta;

        if (stepDelta == 0)
        {
            obs_source_release(filter);
            return;
        }

        auto range = mapping.maxValue - mapping.minValue;
        if (range <= 0.0)
        {
            obs_source_release(filter);
            return;
        }

        auto stepSize = (double(stepDelta) / 127.0) * mapping.scale * range;
        nextValue = juce::jlimit(mapping.minValue, mapping.maxValue, currentValue + stepSize);
    }
    else if (mapping.interactionMode == midi_control_interaction_mode_absolute)
    {
        auto absoluteValue = getAbsoluteValue(mapping, event.message);

        auto pickupKey = mapping.mappingId.toStdString()
                       + "::"
                       + selection.filterUuid.toStdString()
                       + "::"
                       + selection.parameterKey.toStdString();
        auto& pickupState = pickupStates[pickupKey];
        auto currentParameterStep = toPickupStep(mapping, currentValue);
        auto currentControlStep = toPickupStep(mapping, absoluteValue);

        if (pickupState.targetUuid != juce::String(pickupKey))
        {
            pickupState.targetUuid = juce::String(pickupKey);
            pickupState.armed = true;
            pickupState.lastSourceStep = -1;
        }

        if (pickupState.lastSourceStep >= 0 && currentParameterStep != pickupState.lastSourceStep)
            pickupState.armed = true;

        if (pickupState.armed)
        {
            if (shouldDisarmPickup(currentParameterStep, currentControlStep))
                pickupState.armed = false;
            else
            {
                obs_source_release(filter);
                return;
            }
        }

        pickupState.lastSourceStep = currentParameterStep;
        nextValue = absoluteValue;
        pickupState.lastSourceStep = currentControlStep;
    }
    else
    {
        if (!shouldActivateFromEvent(mapping, event.message))
        {
            obs_source_release(filter);
            return;
        }

        if (mapping.interactionMode == midi_control_interaction_mode_toggle)
        {
            auto key = mapping.mappingId.toStdString()
                     + "::"
                     + resolvedLayerToken.toStdString()
                     + "::"
                     + mapping.targetParameterToken.toStdString();
            auto stateValue = !toggleStates[key];
            toggleStates[key] = stateValue;
            nextValue = stateValue ? 1.0 : 0.0;
        }
        else
        {
            nextValue = 1.0;
        }

        if (mapping.valueInverted)
            nextValue = 1.0 - nextValue;
    }

    nextValue = juce::jlimit(0.0, 1.0, nextValue);
    if (std::abs(nextValue - currentValue) < 1.0e-6)
    {
        obs_source_release(filter);
        return;
    }

    auto updated = false;
    if (hasPluginParameterToken && pluginFilter)
        updated = writePluginParameterNormalizedValue(mapping, filter, selection.parameterKey, nextValue);
    else
    {
        ObsFilterLastTouchedEntry rateLimitedEntry;
        if (buildObsFilterRateLimitedEntry(mapping, filter, selection.parameterKey, rateLimitedEntry))
            updated =
                ObsFilterLastTouchedTracker::getInstance().setParameterNormalizedValue(rateLimitedEntry, nextValue);
        else
            updated = writeFilterPropertyNormalizedValue(filter, selection.parameterKey, nextValue);
    }

    obs_source_release(filter);

    if (updated)
    {
        touchFeedbackState(mapping);
        requestFeedbackRefresh();
    }
}

void MidiControlController::handleSourceVolumeMapping(
    const MidiControlMapping& mapping,
    const MidiInputEvent& event,
    obs_source_t* source
)
{
    auto currentVolume = obs_source_get_volume(source);

    auto currentSourceDeflection = double(currentVolume);
    if (volumeFader != nullptr)
    {
        obs_fader_set_mul(volumeFader, currentVolume);
        currentSourceDeflection = double(obs_fader_get_deflection(volumeFader));
    }

    if (isIncrementDecrementMode(mapping.interactionMode))
    {
        auto stepDelta = getRelativeStepDelta(mapping.interactionMode, getMidiValueFromMessage(event.message));

        if (mapping.valueInverted)
            stepDelta = -stepDelta;

        if (stepDelta == 0)
            return;

        auto range = mapping.maxValue - mapping.minValue;
        if (range <= 0.0)
            return;

        auto stepSize = (double(stepDelta) / 127.0) * mapping.scale * range;
        auto nextDeflection = currentSourceDeflection + stepSize;
        nextDeflection = juce::jlimit(mapping.minValue, mapping.maxValue, nextDeflection);

        if (volumeFader != nullptr)
        {
            obs_fader_set_deflection(volumeFader, float(nextDeflection));
            auto volume = obs_fader_get_mul(volumeFader);
            obs_source_set_volume(source, volume);
        }
        else
        {
            obs_source_set_volume(source, float(nextDeflection));
        }

        touchFeedbackState(mapping);
        return;
    }

    auto deflection = float(getAbsoluteValue(mapping, event.message));

    auto pickupKey = mapping.mappingId.toStdString();
    auto currentTargetUuid = juce::String(obs_source_get_uuid(source));

    auto currentSourceStep = toPickupStep(mapping, currentSourceDeflection);
    auto currentControlStep = toPickupStep(mapping, deflection);

    auto& pickupState = pickupStates[pickupKey];

    if (pickupState.targetUuid != currentTargetUuid)
    {
        pickupState.targetUuid = currentTargetUuid;
        pickupState.armed = true;
        pickupState.lastSourceStep = -1;
    }

    if (pickupState.lastSourceStep >= 0 && currentSourceStep != pickupState.lastSourceStep)
        pickupState.armed = true;

    if (pickupState.armed && volumeFader != nullptr)
    {
        if (shouldDisarmPickup(currentSourceStep, currentControlStep))
            pickupState.armed = false;
        else
            return;
    }

    pickupState.lastSourceStep = currentSourceStep;

    if (volumeFader != nullptr)
    {
        obs_fader_set_deflection(volumeFader, deflection);
        auto volume = obs_fader_get_mul(volumeFader);
        obs_source_set_volume(source, volume);
        pickupState.lastSourceStep = currentControlStep;
    }
    else
    {
        obs_source_set_volume(source, deflection);
    }

    touchFeedbackState(mapping);
}

void MidiControlController::handleSourceDiscreteMapping(
    const MidiControlMapping& mapping,
    const MidiInputEvent& event,
    obs_source_t* source
)
{
    int sourceActionType = midi_control_source_action_type_none;
    auto hasSourceActionType = mapping.actionType == midi_control_action_type_source
                            && parseMidiControlSourceActionType(mapping.targetLayerToken, sourceActionType);

    if (mapping.interactionMode == midi_control_interaction_mode_absolute)
    {
        auto value = getAbsoluteSwitchValue(mapping, event.message);

        if (hasSourceActionType && sourceActionType == midi_control_source_action_type_mute)
        {
            obs_source_set_muted(source, value);
            touchFeedbackState(mapping);
        }
        else if (hasSourceActionType && sourceActionType == midi_control_source_action_type_monitoring)
        {
            setSourceMonitoringEnabled(source, value);
            touchFeedbackState(mapping);
        }
        else if (mapping.actionType == midi_control_action_type_media)
        {
            int mediaActionType = midi_control_media_action_type_none;
            if (!parseMidiControlMediaActionType(mapping.targetLayerToken, mediaActionType))
                return;

            if (mediaActionType == midi_control_media_action_type_play_pause)
                obs_source_media_play_pause(source, !value);
            else if (mediaActionType == midi_control_media_action_type_restart)
            {
                if (value)
                    obs_source_media_restart(source);
            }
            else if (mediaActionType == midi_control_media_action_type_stop)
            {
                if (value)
                    obs_source_media_stop(source);
            }
        }

        return;
    }

    if (!shouldActivateFromEvent(mapping, event.message))
        return;

    auto isToggleMode = mapping.interactionMode == midi_control_interaction_mode_toggle;
    auto stateValue = true;

    if (isToggleMode)
    {
        auto key = mapping.mappingId.toStdString();
        stateValue = !toggleStates[key];
        toggleStates[key] = stateValue;
    }

    if (hasSourceActionType && sourceActionType == midi_control_source_action_type_mute)
    {
        obs_source_set_muted(source, stateValue);
        touchFeedbackState(mapping);
    }
    else if (hasSourceActionType && sourceActionType == midi_control_source_action_type_monitoring)
    {
        setSourceMonitoringEnabled(source, stateValue);
        touchFeedbackState(mapping);
    }
    else if (mapping.actionType == midi_control_action_type_media)
    {
        int mediaActionType = midi_control_media_action_type_none;
        if (!parseMidiControlMediaActionType(mapping.targetLayerToken, mediaActionType))
            return;

        if (mediaActionType == midi_control_media_action_type_play_pause)
            obs_source_media_play_pause(source, isToggleMode ? !stateValue : false);
        else if (mediaActionType == midi_control_media_action_type_restart)
            obs_source_media_restart(source);
        else if (mediaActionType == midi_control_media_action_type_stop)
            obs_source_media_stop(source);
    }
}

void MidiControlController::touchFeedbackState(const MidiControlMapping& mapping)
{
    suppressFeedbackForMapping(mapping);
}

void MidiControlController::suppressFeedbackForMapping(const MidiControlMapping& mapping)
{
    auto key = mapping.mappingId.toStdString();
    auto nowMs = juce::Time::getMillisecondCounterHiRes();
    feedbackStates[key].suppressUntilMs = nowMs + feedbackSuppressionWindowMs;
    pendingFeedbackMappingIds.insert(key);
}

void MidiControlController::rebuildSourceObservers()
{
    clearSourceObservers();

    for (auto& mapping : mappings)
    {
        if (!mapping.enabled)
            continue;

        if (!isFeedbackAction(mapping.actionType))
            continue;

        if (mapping.actionType == midi_control_action_type_last_touched
            || mapping.actionType == midi_control_action_type_mapping_context
            || mapping.actionType == midi_control_action_type_effect_parm)
            continue;

        if (!isMappingWinnerForCurrentContext(mapping))
            continue;

        auto* source = findTargetSource(mapping);
        if (source == nullptr)
            continue;

        auto sourceUuid = juce::String(obs_source_get_uuid(source));
        auto observerKey = sourceUuid.toStdString();

        auto it = sourceObservers.find(observerKey);
        if (it == sourceObservers.end())
        {
            MidiControlSourceObserver observer;
            observer.sourceUuid = sourceUuid;
            it = sourceObservers.emplace(observerKey, observer).first;
        }

        obs_source_release(source);

        if (mapping.actionType == midi_control_action_type_source)
        {
            int sourceActionType = midi_control_source_action_type_none;
            if (!parseMidiControlSourceActionType(mapping.targetLayerToken, sourceActionType))
                continue;

            if (sourceActionType == midi_control_source_action_type_volume)
                it->second.observeVolume = true;
            else if (sourceActionType == midi_control_source_action_type_mute)
                it->second.observeMute = true;
            else if (sourceActionType == midi_control_source_action_type_monitoring)
                it->second.observeMonitoring = true;
        }
    }

    for (auto& observerEntry : sourceObservers)
    {
        auto& observer = observerEntry.second;
        auto* source = obs_get_source_by_uuid(observer.sourceUuid.toRawUTF8());
        if (source == nullptr)
            continue;

        auto* signalHandler = obs_source_get_signal_handler(source);
        if (signalHandler == nullptr)
        {
            obs_source_release(source);
            continue;
        }

        if (observer.observeVolume)
            signal_handler_connect(signalHandler, "volume", onObservedSourceSignal, this);

        if (observer.observeMute)
            signal_handler_connect(signalHandler, "mute", onObservedSourceSignal, this);

        if (observer.observeMonitoring)
            signal_handler_connect(signalHandler, "audio_monitoring", onObservedSourceSignal, this);

        obs_source_release(source);
    }
}

void MidiControlController::clearSourceObservers()
{
    for (auto& observerEntry : sourceObservers)
    {
        auto& observer = observerEntry.second;

        auto* source = obs_get_source_by_uuid(observer.sourceUuid.toRawUTF8());
        if (source == nullptr)
            continue;

        auto* signalHandler = obs_source_get_signal_handler(source);
        if (signalHandler != nullptr)
        {
            signal_handler_disconnect(signalHandler, "volume", onObservedSourceSignal, this);
            signal_handler_disconnect(signalHandler, "mute", onObservedSourceSignal, this);
            signal_handler_disconnect(signalHandler, "audio_monitoring", onObservedSourceSignal, this);
        }

        obs_source_release(source);
    }

    sourceObservers.clear();
}

void MidiControlController::enqueueAllMappingsForFeedback()
{
    for (auto& mapping : mappings)
        enqueueMappingForFeedback(mapping.mappingId);
}

void MidiControlController::enqueueLastTouchedMappingsForFeedback()
{
    for (auto& mapping : mappings)
        if (mapping.actionType == midi_control_action_type_last_touched && isMappingWinnerForCurrentContext(mapping))
            enqueueMappingForFeedback(mapping.mappingId);
}

void MidiControlController::enqueueEffectParmMappingsForFeedback()
{
    for (auto& mapping : mappings)
        if (mapping.actionType == midi_control_action_type_effect_parm && isMappingWinnerForCurrentContext(mapping))
            enqueueMappingForFeedback(mapping.mappingId);
}

void MidiControlController::enqueueMappingForFeedback(const juce::String& mappingId)
{
    if (mappingId.isEmpty())
        return;

    pendingFeedbackMappingIds.insert(mappingId.toStdString());
}

bool MidiControlController::isFeedbackAction(int actionType) const
{
    return actionType == midi_control_action_type_source
        || actionType == midi_control_action_type_mapping_context
        || actionType == midi_control_action_type_last_touched
        || actionType == midi_control_action_type_effect_parm;
}

bool MidiControlController::isMappingEligibleForFeedbackInCurrentContext(const MidiControlMapping& mapping) const
{
    if (!isFeedbackAction(mapping.actionType))
        return false;

    if (mapping.actionType == midi_control_action_type_mapping_context)
        return true;

    if (mapping.actionType == midi_control_action_type_last_touched
        || mapping.actionType == midi_control_action_type_effect_parm)
    {
        return isMappingWinnerForCurrentContext(mapping);
    }

    return true;
}

int MidiControlController::getMappingContextSpecificity(const MidiControlMapping& mapping) const
{
    auto bankMatches = mapping.bank == midi_control_mapping_context_any || mapping.bank == activeBank;
    if (!bankMatches)
        return -1;

    auto presetMatches = mapping.preset == midi_control_mapping_context_any || mapping.preset == activePreset;
    if (!presetMatches)
        return -1;

    auto specificity = 0;
    if (mapping.bank == activeBank)
        specificity += 1;

    if (mapping.preset == activePreset)
        specificity += 1;

    return specificity;
}

bool MidiControlController::hasSameInputSignature(const MidiControlMapping& left, const MidiControlMapping& right) const
{
    return left.inputDeviceName == right.inputDeviceName
        && left.messageType == right.messageType
        && left.channel == right.channel
        && left.data1 == right.data1;
}

bool MidiControlController::isMappingWinnerForCurrentContext(const MidiControlMapping& mapping) const
{
    auto ownSpecificity = getMappingContextSpecificity(mapping);
    if (ownSpecificity < 0)
        return false;

    auto highestSpecificity = ownSpecificity;

    for (auto& candidate : mappings)
    {
        if (!candidate.enabled)
            continue;

        if (candidate.mappingId == mapping.mappingId)
            continue;

        if (!hasSameInputSignature(candidate, mapping))
            continue;

        auto candidateSpecificity = getMappingContextSpecificity(candidate);
        if (candidateSpecificity > highestSpecificity)
            highestSpecificity = candidateSpecificity;
    }

    return ownSpecificity == highestSpecificity;
}

bool MidiControlController::isMappingInActiveContext(const MidiControlMapping& mapping) const
{
    return getMappingContextSpecificity(mapping) >= 0;
}

void MidiControlController::applyMappingContextChange()
{
    toggleStates.clear();
    pickupStates.clear();
    transitionStates.clear();
    clearDelayedFeedbackQueue();
    feedbackSyncPending = false;
    feedbackSyncDeadlineMs = 0.0;

    if (!isRuntimeActive())
        return;

    rebuildSourceObservers();
    enqueueAllMappingsForFeedback();

    MidiControlActiveContextDock::refreshIfOpen();
    MidiControlLastTouchedHelperDialog::refreshIfOpen();
}

bool MidiControlController::isFeedbackRouteActive(const MidiControlMapping& mapping) const
{
    if (mapping.outputMode == midi_control_output_mode_none)
        return false;

    if (mapping.outputMode == midi_control_output_mode_any)
        return !subscriptions.subscribedOutputDevices.isEmpty();

    if (mapping.outputMode == midi_control_output_mode_specific)
        return mapping.outputDeviceName.isNotEmpty();

    failFast(
        "MidiControlController::isFeedbackRouteActive",
        "unsupported outputMode " + juce::String(mapping.outputMode) + " for mapping " + mapping.mappingId
    );
}

void MidiControlController::onObservedSourceSignal(void* privateData, calldata_t*)
{
    auto* self = static_cast<MidiControlController*>(privateData);
    if (self == nullptr)
        return;

    self->requestFeedbackRefresh();
}

void MidiControlController::sendFeedbackMessage(const MidiControlMapping& mapping, double normalizedValue)
{
    if (mapping.outputMode == midi_control_output_mode_none)
        return;

    auto message = createFeedbackMidiMessage(mapping, normalizedValue);
    juce::MidiBuffer buffer;
    buffer.addEvent(message, 0);

    if (mapping.outputMode == midi_control_output_mode_specific)
    {
        if (mapping.outputDeviceName.isNotEmpty())
            midiClient.sendMidiToDevice(buffer, mapping.outputDeviceName);

        return;
    }

    midiClient.sendMidi(buffer);
}

void MidiControlController::queueDelayedFeedback(
    const MidiControlMapping& mapping,
    double normalizedValue,
    bool inactiveClear
)
{
    auto key = mapping.mappingId.toStdString();
    auto& pending = delayedFeedbackByMappingId[key];
    pending.mapping = mapping;
    pending.normalizedValue = normalizedValue;
    pending.inactiveClear = inactiveClear;
    delayedFeedbackOutputIdleDeadlineMs =
        juce::Time::getMillisecondCounterHiRes() + double(delayedFeedbackOutputIdleMs);
}

void MidiControlController::clearDelayedFeedbackQueue()
{
    delayedFeedbackByMappingId.clear();
    delayedFeedbackOutputIdleDeadlineMs = 0.0;
}

int MidiControlController::clampDelayedFeedbackOutputIdleMs(int delayMs) const
{
    return juce::jlimit(delayedFeedbackOutputMinIdleMs, delayedFeedbackOutputMaxIdleMs, delayMs);
}

double MidiControlController::getObservedActionValue(const MidiControlMapping& mapping, obs_source_t* source)
{
    if (mapping.actionType == midi_control_action_type_effect_parm)
    {
        EffectParmSelection selection;
        if (!parseEffectParmObsFilterSelection(mapping, selection))
            return 0.0;

        auto resolvedLayerToken = mapping.targetLayerToken;
        auto* filter = findFilterByUuidOnSource(source, selection.filterUuid);

        if (filter == nullptr && matchByNameEnabled && mapping.targetLayerName.isNotEmpty())
        {
            filter = findFilterByNameOnSource(source, mapping.targetLayerName);
            if (filter != nullptr)
            {
                auto* resolvedFilterUuid = obs_source_get_uuid(filter);
                if (resolvedFilterUuid != nullptr && resolvedFilterUuid[0] != '\0')
                {
                    selection.filterUuid = juce::String(resolvedFilterUuid);
                    resolvedLayerToken = createEffectParmLayerToken(selection.filterUuid);
                }
            }
        }

        if (filter == nullptr)
            return 0.0;

        if (resolvedLayerToken.isNotEmpty() && resolvedLayerToken != mapping.targetLayerToken)
            applyMappingAutoHeal(mapping, {}, resolvedLayerToken);

        double normalizedValue = 0.0;
        int pluginParameterIndex = -1;
        auto hasPluginParameterToken = parsePluginParameterIndexToken(selection.parameterKey, pluginParameterIndex);
        juce::ignoreUnused(pluginParameterIndex);
        auto ok = false;

        if (hasPluginParameterToken && isPluginHostFilter(filter))
        {
            ok = readPluginParameterNormalizedValue(mapping, filter, selection.parameterKey, normalizedValue);
        }
        else
        {
            auto filterUuid = juce::String(obs_source_get_uuid(filter));
            auto identity = mapping.targetUuid + "::" + filterUuid + "::" + selection.parameterKey;
            ObsFilterLastTouchedEntry cachedEntry;
            if (ObsFilterLastTouchedTracker::getInstance().getParameterByIdentity(identity, cachedEntry))
            {
                normalizedValue = cachedEntry.normalizedValue;
                ok = true;
            }
            else
            {
                ok = readFilterPropertyNormalizedValue(filter, selection.parameterKey, normalizedValue);
            }
        }

        obs_source_release(filter);
        if (!ok)
            return 0.0;

        return normalizedValue;
    }

    if (mapping.actionType == midi_control_action_type_source)
    {
        int sourceActionType = midi_control_source_action_type_none;
        if (!parseMidiControlSourceActionType(mapping.targetLayerToken, sourceActionType))
            return 0.0;

        if (sourceActionType == midi_control_source_action_type_mute)
            return obs_source_muted(source) ? 1.0 : 0.0;

        if (sourceActionType == midi_control_source_action_type_monitoring)
            return obs_source_get_monitoring_type(source) != OBS_MONITORING_TYPE_NONE ? 1.0 : 0.0;
    }

    auto volume = obs_source_get_volume(source);

    if (feedbackVolumeFader == nullptr)
        return juce::jlimit(0.0, 1.0, double(volume));

    obs_fader_set_mul(feedbackVolumeFader, volume);
    return juce::jlimit(0.0, 1.0, double(obs_fader_get_deflection(feedbackVolumeFader)));
}

double MidiControlController::toMidiNormalizedValue(const MidiControlMapping& mapping, double observedValue)
{
    return toMidiNormalizedFromObservedValue(mapping, observedValue);
}

juce::MidiMessage
MidiControlController::createFeedbackMidiMessage(const MidiControlMapping& mapping, double normalizedValue)
{
    auto channel = mapping.channel > 0 ? mapping.channel : 1;
    channel = juce::jlimit(1, 16, channel);
    auto midiValue = juce::jlimit(0, 127, int(std::lround(normalizedValue * 127.0)));

    if (mapping.messageType == midi_control_message_type_program_change)
        return juce::MidiMessage::programChange(channel, midiValue);

    if (mapping.messageType == midi_control_message_type_note)
    {
        if (midiValue <= 0)
            return juce::MidiMessage::noteOff(channel, mapping.data1, (juce::uint8)0);

        return juce::MidiMessage::noteOn(channel, mapping.data1, (juce::uint8)midiValue);
    }

    if (mapping.messageType == midi_control_message_type_cc)
        return juce::MidiMessage::controllerEvent(channel, mapping.data1, midiValue);

    failFast(
        "MidiControlController::createFeedbackMidiMessage",
        "unsupported messageType " + juce::String(mapping.messageType) + " for mapping " + mapping.mappingId
    );
}

void MidiControlController::applyMappingAutoHeal(
    const MidiControlMapping& mapping,
    juce::String resolvedTargetUuid,
    juce::String resolvedTargetLayerToken
)
{
    auto mappingIndex = -1;
    for (int i = 0; i < int(mappings.size()); ++i)
    {
        if (mappings[size_t(i)].mappingId == mapping.mappingId)
        {
            mappingIndex = i;
            break;
        }
    }

    if (mappingIndex < 0)
        return;

    auto& storedMapping = mappings[size_t(mappingIndex)];
    auto changed = false;

    if (resolvedTargetUuid.isNotEmpty() && storedMapping.targetUuid != resolvedTargetUuid)
    {
        storedMapping.targetUuid = resolvedTargetUuid;
        changed = true;
    }

    if (resolvedTargetLayerToken.isNotEmpty() && storedMapping.targetLayerToken != resolvedTargetLayerToken)
    {
        storedMapping.targetLayerToken = resolvedTargetLayerToken;
        changed = true;
    }

    if (!changed)
        return;

    if (isRuntimeActive() && !initialStateRestorePending)
    {
        sourceObserverRebuildPending = true;
        enqueueMappingForFeedback(storedMapping.mappingId);
    }

    atk::settings::setMidiControlMappings(mappings);
}

obs_source_t* MidiControlController::findTargetSource(const MidiControlMapping& mapping)
{
    auto sourceResult = resolveTargetSource(mapping, matchByNameEnabled);
    if (sourceResult.source == nullptr)
        return nullptr;

    if (sourceResult.usedNameFallback && sourceResult.resolvedUuid.isNotEmpty())
        applyMappingAutoHeal(mapping, sourceResult.resolvedUuid, {});

    return sourceResult.source;
}

double
MidiControlController::getAbsoluteValue(const MidiControlMapping& mapping, const juce::MidiMessage& message) const
{
    auto normalizedValue = getMidiValueFromMessage(message) / 127.0;

    return toObservedValueFromMidiNormalized(mapping, normalizedValue);
}

bool MidiControlController::getAbsoluteSwitchValue(
    const MidiControlMapping& mapping,
    const juce::MidiMessage& message
) const
{
    return getAbsoluteValue(mapping, message) >= 0.5;
}

bool MidiControlController::shouldActivateFromEvent(
    const MidiControlMapping& mapping,
    const juce::MidiMessage& message
) const
{
    if (mapping.interactionMode == midi_control_interaction_mode_absolute)
        return getAbsoluteSwitchValue(mapping, message);

    return shouldTrigger(message, mapping.messageType);
}

bool MidiControlController::shouldTrigger(const juce::MidiMessage& message, int messageType) const
{
    if (messageType == midi_control_message_type_cc)
        return message.isController() && message.getControllerValue() > 63;

    if (messageType == midi_control_message_type_note)
        return message.isNoteOn();

    if (messageType == midi_control_message_type_program_change)
        return message.isProgramChange();

    failFast("MidiControlController::shouldTrigger", "unsupported messageType " + juce::String(messageType));
}

} // namespace atk
