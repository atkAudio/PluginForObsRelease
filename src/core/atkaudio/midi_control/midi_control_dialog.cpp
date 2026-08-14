#include "midi_control_dialog.h"

#include "midi_control_active_context_dock.h"
#include "midi_control_mappings_table_model.h"
#include "midi_control_last_touched_helper_dialog.h"

#include <atkaudio/midi_control/last_touched_parameter_tracker.h>
#include <atkaudio/midi_control/midi_control_controller.h>

#include <obs-frontend-api.h>
#include <obs-hotkey.h>

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <array>
#include <algorithm>
#include <string_view>
#include <unordered_set>

namespace
{
enum MidiControlTableColumn
{
    midi_control_column_enabled = 0,
    midi_control_column_learn,
    midi_control_column_input_device,
    midi_control_column_output_route,
    midi_control_column_type,
    midi_control_column_channel,
    midi_control_column_data1,
    midi_control_column_mode,
    midi_control_column_action,
    midi_control_column_target,
    midi_control_column_layer,
    midi_control_column_parameter,
    midi_control_column_bank,
    midi_control_column_preset,
    midi_control_column_activation,
    midi_control_column_invert,
    midi_control_column_min,
    midi_control_column_max,
    midi_control_column_scale,
    midi_control_column_count,
};

struct ObsSourceEntry
{
    juce::String uuid;
    juce::String name;
};

struct ObsSceneEntry
{
    juce::String uuid;
    juce::String name;
};

struct ObsFilterEntry
{
    juce::String uuid;
    juce::String name;
    juce::String sourceId;
};

struct ObsFilterParameterEntry
{
    juce::String key;
    juce::String displayName;
};

struct ObsSourceScanContext
{
    std::vector<ObsSourceEntry>* entries = nullptr;
    bool controllableMediaOnly = false;
};

struct HotkeyOptionsBuildContext
{
    std::vector<atk::MidiControlTargetOption>* options = nullptr;
};

constexpr auto previousSceneTargetName = "Previous Scene";
constexpr auto nextSceneTargetName = "Next Scene";
constexpr auto transitionTbarTargetName = "TBar";
constexpr auto transitionTriggerTargetName = "Trigger";
constexpr auto outputRouteNoneValue = "__none__";
constexpr auto outputRouteAnyValue = "__any__";
constexpr auto mappingsFilePattern = "*.json";
constexpr std::string_view effectParmObsFilterLayerPrefix = "effect_parm_obs_filter:";
constexpr std::string_view pluginHostFilterId = "atkaudio_plugin_host";
constexpr std::string_view pluginHost2FilterId = "atkaudio_plugin_host2";
constexpr std::string_view effectParmIndexPrefix = "effect_parm_index:";

QString toQString(juce::String text)
{
    return QString::fromUtf8(text.toRawUTF8());
}

QString getActiveContextDisplayText(atk::MidiControlController* controller)
{
    if (controller == nullptr)
        return "Active Bank/Preset: unavailable";

    return QString::fromUtf8("Active Bank/Preset: ")
         + QString::number(controller->getActiveBank())
         + "/"
         + QString::number(controller->getActivePreset());
}

juce::String toJuceString(QString text)
{
    return juce::String::fromUTF8(text.toUtf8().constData());
}

juce::File getDefaultMappingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("atkaudio-midi-to-obs-mappings.json");
}

bool isSceneAction(int actionType)
{
    return actionType == atk::midi_control_action_type_scene;
}

bool isNoneAction(int actionType)
{
    return actionType == atk::midi_control_action_type_none;
}

bool isTransitionAction(int actionType)
{
    return actionType == atk::midi_control_action_type_transition;
}

bool isHotkeyAction(int actionType)
{
    return actionType == atk::midi_control_action_type_hotkey;
}

bool isLastTouchedAction(int actionType)
{
    return actionType == atk::midi_control_action_type_last_touched;
}

bool isMappingContextAction(int actionType)
{
    return actionType == atk::midi_control_action_type_mapping_context;
}

bool isEffectParmAction(int actionType)
{
    return actionType == atk::midi_control_action_type_effect_parm;
}

bool isSourceAction(int actionType)
{
    return actionType == atk::midi_control_action_type_source;
}

bool isMediaAction(int actionType)
{
    return actionType == atk::midi_control_action_type_media;
}

bool isControllableMediaSource(obs_source_t* source)
{
    if (source == nullptr)
        return false;

    auto flags = obs_source_get_output_flags(source);
    return (flags & OBS_SOURCE_CONTROLLABLE_MEDIA) != 0;
}

std::vector<ObsSourceEntry> getObsSources(bool controllableMediaOnly = false)
{
    std::vector<ObsSourceEntry> sources;
    ObsSourceScanContext scanContext;
    scanContext.entries = &sources;
    scanContext.controllableMediaOnly = controllableMediaOnly;

    obs_enum_sources(
        [](void* context, obs_source_t* source)
        {
            auto* scanContext = static_cast<ObsSourceScanContext*>(context);
            if (scanContext == nullptr || scanContext->entries == nullptr)
                return true;

            if (scanContext->controllableMediaOnly && !isControllableMediaSource(source))
                return true;

            auto* entries = scanContext->entries;
            ObsSourceEntry entry;
            entry.uuid = juce::String(obs_source_get_uuid(source));
            entry.name = juce::String(obs_source_get_name(source));

            if (entry.name.isNotEmpty())
                entries->push_back(entry);

            return true;
        },
        &scanContext
    );

    std::sort(
        sources.begin(),
        sources.end(),
        [](const ObsSourceEntry& left, const ObsSourceEntry& right)
        { return left.name.compareIgnoreCase(right.name) < 0; }
    );

    return sources;
}

std::vector<ObsSceneEntry> getObsScenes()
{
    std::vector<ObsSceneEntry> scenes;

    obs_frontend_source_list sceneList = {};
    obs_frontend_get_scenes(&sceneList);

    scenes.reserve(sceneList.sources.num);

    for (size_t i = 0; i < sceneList.sources.num; ++i)
    {
        auto* source = sceneList.sources.array[i];
        if (source == nullptr)
            continue;

        ObsSceneEntry entry;
        entry.uuid = juce::String(obs_source_get_uuid(source));
        entry.name = juce::String(obs_source_get_name(source));

        if (entry.name.isNotEmpty())
            scenes.push_back(entry);
    }

    obs_frontend_source_list_free(&sceneList);

    std::sort(
        scenes.begin(),
        scenes.end(),
        [](const ObsSceneEntry& left, const ObsSceneEntry& right)
        { return left.name.compareIgnoreCase(right.name) < 0; }
    );

    return scenes;
}

bool isWritableFilterPropertyForDirectMapping(obs_property_t* property)
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

bool isPluginHostFilterId(juce::String filterId)
{
    return filterId.equalsIgnoreCase(pluginHostFilterId.data())
        || filterId.equalsIgnoreCase(pluginHost2FilterId.data());
}

std::vector<ObsFilterEntry> getObsSourceFilters(juce::String sourceUuid)
{
    std::vector<ObsFilterEntry> filters;

    if (sourceUuid.isEmpty())
        return filters;

    auto* source = obs_get_source_by_uuid(sourceUuid.toRawUTF8());
    if (source == nullptr)
        return filters;

    obs_source_enum_filters(
        source,
        [](obs_source_t*, obs_source_t* filter, void* privateData)
        {
            auto* filters = static_cast<std::vector<ObsFilterEntry>*>(privateData);
            if (filters == nullptr || filter == nullptr)
                return;

            auto* filterUuid = obs_source_get_uuid(filter);
            auto* filterName = obs_source_get_name(filter);
            auto* filterSourceId = obs_source_get_id(filter);
            if (filterUuid == nullptr || filterName == nullptr)
                return;

            ObsFilterEntry entry;
            entry.uuid = filterUuid;
            entry.name = filterName;
            entry.sourceId = juce::String(filterSourceId != nullptr ? filterSourceId : "");

            if (entry.uuid.isNotEmpty() && entry.name.isNotEmpty())
                filters->push_back(entry);
        },
        &filters
    );

    obs_source_release(source);

    std::sort(
        filters.begin(),
        filters.end(),
        [](const ObsFilterEntry& left, const ObsFilterEntry& right)
        { return left.name.compareIgnoreCase(right.name) < 0; }
    );

    return filters;
}

juce::String parseEffectParmObsFilterUuidFromLayerToken(juce::String token)
{
    if (!token.startsWithIgnoreCase(effectParmObsFilterLayerPrefix.data()))
        return {};

    auto filterUuid = token.fromFirstOccurrenceOf(effectParmObsFilterLayerPrefix.data(), false, false).trim();
    if (filterUuid.isEmpty())
        return {};

    return filterUuid;
}

juce::String createEffectParmIndexToken(int parameterIndex)
{
    if (parameterIndex < 0)
        return {};

    return juce::String(effectParmIndexPrefix.data()) + juce::String(parameterIndex);
}

bool getObsFilterEntryByUuid(juce::String filterUuid, ObsFilterEntry& entry)
{
    entry = {};

    if (filterUuid.isEmpty())
        return false;

    auto* filter = obs_get_source_by_uuid(filterUuid.toRawUTF8());
    if (filter == nullptr)
        return false;

    auto* uuid = obs_source_get_uuid(filter);
    auto* name = obs_source_get_name(filter);
    auto* sourceId = obs_source_get_id(filter);

    if (uuid != nullptr)
        entry.uuid = uuid;

    if (name != nullptr)
        entry.name = name;

    if (sourceId != nullptr)
        entry.sourceId = sourceId;

    obs_source_release(filter);
    return entry.uuid.isNotEmpty() && entry.name.isNotEmpty();
}

juce::String getPluginParameterDisplayName(const atk::LastTouchedParameterEntry& entry)
{
    auto parameterName = entry.parameter != nullptr ? entry.parameter->getName(128).trim() : juce::String();
    if (parameterName.isEmpty())
        parameterName = "Parameter " + juce::String(entry.parameterIndex);

    auto processorName = entry.processor != nullptr ? entry.processor->getName().trim() : juce::String();
    if (processorName.isNotEmpty())
        return processorName + " :: " + parameterName;

    return parameterName;
}

std::vector<ObsFilterParameterEntry> getObsFilterParameterEntries(juce::String sourceUuid, juce::String filterUuid)
{
    std::vector<ObsFilterParameterEntry> parameters;

    if (sourceUuid.isEmpty() || filterUuid.isEmpty())
        return parameters;

    ObsFilterEntry filterEntry;
    if (!getObsFilterEntryByUuid(filterUuid, filterEntry))
        return parameters;

    if (isPluginHostFilterId(filterEntry.sourceId))
    {
        auto entries =
            atk::LastTouchedParameterTracker::getInstance().getParametersForOwner(sourceUuid, filterEntry.name);
        std::unordered_set<std::string> seenKeys;

        for (auto& trackerEntry : entries)
        {
            if (trackerEntry.parameterIndex < 0)
                continue;

            ObsFilterParameterEntry entry;
            entry.key = createEffectParmIndexToken(trackerEntry.parameterIndex);
            entry.displayName = getPluginParameterDisplayName(trackerEntry);

            if (entry.key.isEmpty() || entry.displayName.isEmpty())
                continue;

            auto key = entry.key.toStdString();
            if (seenKeys.find(key) != seenKeys.end())
                continue;

            seenKeys.insert(std::move(key));
            parameters.push_back(entry);
        }

        std::sort(
            parameters.begin(),
            parameters.end(),
            [](const ObsFilterParameterEntry& left, const ObsFilterParameterEntry& right)
            { return left.displayName.compareIgnoreCase(right.displayName) < 0; }
        );

        return parameters;
    }

    auto* filter = obs_get_source_by_uuid(filterUuid.toRawUTF8());
    if (filter == nullptr)
        return parameters;

    auto* properties = obs_source_properties(filter);
    if (properties == nullptr)
    {
        obs_source_release(filter);
        return parameters;
    }

    auto* property = obs_properties_first(properties);
    while (property != nullptr)
    {
        if (isWritableFilterPropertyForDirectMapping(property))
        {
            auto* key = obs_property_name(property);
            auto name = juce::String(key != nullptr ? key : "");
            if (name.isNotEmpty())
            {
                ObsFilterParameterEntry entry;
                entry.key = name;
                entry.displayName = name;
                parameters.push_back(entry);
            }
        }

        obs_property_next(&property);
    }

    obs_properties_destroy(properties);
    obs_source_release(filter);

    std::sort(
        parameters.begin(),
        parameters.end(),
        [](const ObsFilterParameterEntry& left, const ObsFilterParameterEntry& right)
        { return left.displayName.compareIgnoreCase(right.displayName) < 0; }
    );

    return parameters;
}

std::vector<atk::MidiControlTargetOption> getObsHotkeyTargetOptions()
{
    std::vector<atk::MidiControlTargetOption> options;

    HotkeyOptionsBuildContext buildContext;
    buildContext.options = &options;

    obs_enum_hotkeys(
        [](void* context, obs_hotkey_id id, obs_hotkey_t* key)
        {
            auto* buildContext = static_cast<HotkeyOptionsBuildContext*>(context);
            if (buildContext == nullptr || buildContext->options == nullptr || key == nullptr)
                return true;

            auto registererType = obs_hotkey_get_registerer_type(key);
            auto displayName = "Hotkey #" + juce::String((long long)id);

            if (registererType == OBS_HOTKEY_REGISTERER_FRONTEND)
            {
                auto* descriptionChars = obs_hotkey_get_description(key);
                if (descriptionChars != nullptr)
                {
                    auto description = juce::String(descriptionChars).trim();
                    if (description.isNotEmpty())
                        displayName = description;
                }

                if (displayName.startsWithIgnoreCase("Hotkey #"))
                {
                    auto* nameChars = obs_hotkey_get_name(key);
                    if (nameChars != nullptr)
                    {
                        auto name = juce::String(nameChars).trim();
                        if (name.isNotEmpty())
                            displayName = name;
                    }
                }

                buildContext->options->push_back({
                    atk::createMidiControlHotkeyIdTargetToken(uint64_t(id)),
                    displayName,
                });
                return true;
            }

            return true;
        },
        &buildContext
    );

    std::sort(
        options.begin(),
        options.end(),
        [](const atk::MidiControlTargetOption& left, const atk::MidiControlTargetOption& right)
        { return left.displayName.compareIgnoreCase(right.displayName) < 0; }
    );

    return options;
}

void addMessageTypeOptions(QComboBox* combo)
{
    combo->addItem("CC", atk::midi_control_message_type_cc);
    combo->addItem("Note", atk::midi_control_message_type_note);
    combo->addItem("Program", atk::midi_control_message_type_program_change);
}

void addModeOptions(QComboBox* combo)
{
    combo->addItem("Absolute", atk::midi_control_interaction_mode_absolute);
    combo->addItem("Trigger", atk::midi_control_interaction_mode_trigger);
    combo->addItem("Toggle", atk::midi_control_interaction_mode_toggle);
    combo->addItem("Inc/Dec", atk::midi_control_interaction_mode_inc_dec);
    combo->addItem("Inc/Dec2", atk::midi_control_interaction_mode_inc_dec2);
}

void addActionOptions(QComboBox* combo)
{
    auto actionOptions = atk::getMidiControlActionOptions();

    for (auto& option : actionOptions)
        combo->addItem(toQString(option.displayName), option.actionType);
}

void addActivationOptions(QComboBox* combo)
{
    combo->addItem("Scene", atk::midi_control_activation_mode_scene);
    combo->addItem("Global", atk::midi_control_activation_mode_global);
    combo->addItem("Preview", atk::midi_control_activation_mode_preview);
}

void syncMappingTargetFromCombo(atk::MidiControlMapping& mapping, QComboBox* combo)
{
    if (isNoneAction(mapping.actionType))
    {
        mapping.targetName.clear();
        mapping.targetUuid.clear();
        mapping.targetLayerToken.clear();
        mapping.targetLayerName.clear();
        mapping.targetParameterToken.clear();
        mapping.targetParameterName.clear();
        return;
    }

    mapping.targetName = toJuceString(combo->currentText());

    if (isTransitionAction(mapping.actionType))
    {
        mapping.targetUuid.clear();
        return;
    }

    if (isLastTouchedAction(mapping.actionType) || isMappingContextAction(mapping.actionType))
    {
        mapping.targetUuid = toJuceString(combo->currentData().toString());
        return;
    }

    if (isHotkeyAction(mapping.actionType))
    {
        auto targetToken = toJuceString(combo->currentData().toString()).trim();
        if (targetToken.isEmpty())
            targetToken = toJuceString(combo->currentText()).trim();

        mapping.targetUuid = targetToken;
        mapping.targetName = toJuceString(combo->currentText());
        return;
    }

    if (isSceneAction(mapping.actionType))
    {
        if (mapping.targetName == previousSceneTargetName || mapping.targetName == nextSceneTargetName)
            mapping.targetUuid.clear();
        else
            mapping.targetUuid = toJuceString(combo->currentData().toString());

        return;
    }

    mapping.targetUuid = toJuceString(combo->currentData().toString());

    if (isMediaAction(mapping.actionType) && mapping.targetUuid.isEmpty())
        mapping.targetName.clear();
}

void populateTargetComboForMapping(
    QComboBox* combo,
    atk::MidiControlMapping& mapping,
    const std::vector<ObsSourceEntry>& sources,
    const std::vector<ObsSourceEntry>& mediaSources
)
{
    combo->clear();

    if (isNoneAction(mapping.actionType))
    {
        mapping.targetName.clear();
        mapping.targetUuid.clear();
        return;
    }

    if (isSceneAction(mapping.actionType))
    {
        combo->addItem(nextSceneTargetName, nextSceneTargetName);
        combo->addItem(previousSceneTargetName, previousSceneTargetName);

        auto scenes = getObsScenes();
        for (const auto& scene : scenes)
            combo->addItem(toQString(scene.name), toQString(scene.uuid));
    }
    else if (isTransitionAction(mapping.actionType))
    {
        combo->addItem(transitionTbarTargetName, transitionTbarTargetName);
        combo->addItem(transitionTriggerTargetName, transitionTriggerTargetName);
    }
    else if (isLastTouchedAction(mapping.actionType))
    {
        auto targetOptions = atk::getMidiControlTargetOptionsForAction(mapping.actionType);
        for (auto& option : targetOptions)
            combo->addItem(toQString(option.displayName), toQString(option.targetToken));
    }
    else if (isMappingContextAction(mapping.actionType))
    {
        auto targetOptions = atk::getMidiControlTargetOptionsForAction(mapping.actionType);
        for (auto& option : targetOptions)
            combo->addItem(toQString(option.displayName), toQString(option.targetToken));
    }
    else if (isHotkeyAction(mapping.actionType))
    {
        auto targetOptions = getObsHotkeyTargetOptions();
        for (auto& option : targetOptions)
            combo->addItem(toQString(option.displayName), toQString(option.targetToken));

        combo->setEditable(true);
    }
    else if (isMediaAction(mapping.actionType))
    {
        combo->addItem("Select media source", "");

        for (const auto& source : mediaSources)
            combo->addItem(toQString(source.name), toQString(source.uuid));
    }
    else
    {
        for (const auto& source : sources)
            combo->addItem(toQString(source.name), toQString(source.uuid));
    }

    int selectedIndex = -1;

    for (int i = 0; i < combo->count(); ++i)
    {
        auto itemText = combo->itemText(i);
        auto itemData = combo->itemData(i).toString();

        if (isSceneAction(mapping.actionType))
        {
            if ((mapping.targetName == previousSceneTargetName && itemText == previousSceneTargetName)
                || (mapping.targetName == nextSceneTargetName && itemText == nextSceneTargetName)
                || itemData == toQString(mapping.targetUuid))
            {
                selectedIndex = i;
                break;
            }
        }
        else if (isTransitionAction(mapping.actionType))
        {
            if ((mapping.targetName == transitionTbarTargetName && itemText == transitionTbarTargetName)
                || (mapping.targetName == transitionTriggerTargetName && itemText == transitionTriggerTargetName))
            {
                selectedIndex = i;
                break;
            }
        }
        else if (isLastTouchedAction(mapping.actionType))
        {
            if (itemData == toQString(mapping.targetUuid))
            {
                selectedIndex = i;
                break;
            }
        }
        else if (isMappingContextAction(mapping.actionType))
        {
            if (itemData == toQString(mapping.targetUuid))
            {
                selectedIndex = i;
                break;
            }
        }
        else if (isHotkeyAction(mapping.actionType))
        {
            if (itemData == toQString(mapping.targetUuid))
            {
                selectedIndex = i;
                break;
            }
        }
        else if (itemData == toQString(mapping.targetUuid))
        {
            selectedIndex = i;
            break;
        }
    }

    if (isHotkeyAction(mapping.actionType) && selectedIndex < 0 && mapping.targetUuid.isNotEmpty())
    {
        auto manualLabel = mapping.targetName.isNotEmpty() ? mapping.targetName : mapping.targetUuid;
        combo->addItem(toQString(manualLabel), toQString(mapping.targetUuid));
        selectedIndex = combo->count() - 1;
    }

    if (isMediaAction(mapping.actionType) && selectedIndex < 0)
        selectedIndex = 0;

    if (selectedIndex < 0 && combo->count() > 0)
        selectedIndex = 0;

    if (selectedIndex >= 0)
        combo->setCurrentIndex(selectedIndex);

    syncMappingTargetFromCombo(mapping, combo);
}

bool validateStrictMediaTargets(const std::vector<atk::MidiControlMapping>& mappings, juce::String& errorMessage)
{
    for (auto& mapping : mappings)
    {
        if (!isMediaAction(mapping.actionType))
            continue;

        if (mapping.targetUuid.isEmpty())
            continue;

        auto* source = obs_get_source_by_uuid(mapping.targetUuid.toRawUTF8());
        if (source == nullptr)
        {
            errorMessage = "media mapping target does not exist in mapping " + mapping.mappingId;
            return false;
        }

        auto isControllableMedia = isControllableMediaSource(source);
        obs_source_release(source);

        if (!isControllableMedia)
        {
            errorMessage = "media mapping target is not a controllable media source in mapping " + mapping.mappingId;
            return false;
        }
    }

    return true;
}

void populateLayerComboForMapping(QComboBox* combo, atk::MidiControlMapping& mapping)
{
    combo->clear();

    if (isSourceAction(mapping.actionType))
    {
        auto options = atk::getMidiControlSourceActionTypeOptions();
        for (auto& option : options)
            combo->addItem(toQString(option.displayName), toQString(option.targetToken));

        if (combo->count() == 0)
        {
            combo->addItem("No source actions", "");
            mapping.targetLayerToken.clear();
            mapping.targetLayerName.clear();
            mapping.targetParameterToken.clear();
            mapping.targetParameterName.clear();
            combo->setCurrentIndex(0);
            combo->setEnabled(false);
            return;
        }

        auto selectedIndex = combo->findData(toQString(mapping.targetLayerToken));
        if (selectedIndex < 0)
            selectedIndex = 0;

        combo->setCurrentIndex(selectedIndex);
        combo->setEnabled(true);
        mapping.targetLayerToken = toJuceString(combo->currentData().toString());
        mapping.targetLayerName = toJuceString(combo->currentText());
        mapping.targetParameterToken.clear();
        mapping.targetParameterName.clear();
        return;
    }

    if (isMediaAction(mapping.actionType))
    {
        auto options = atk::getMidiControlMediaActionTypeOptions();
        for (auto& option : options)
            combo->addItem(toQString(option.displayName), toQString(option.targetToken));

        if (combo->count() == 0)
        {
            combo->addItem("No media actions", "");
            mapping.targetLayerToken.clear();
            mapping.targetLayerName.clear();
            mapping.targetParameterToken.clear();
            mapping.targetParameterName.clear();
            combo->setCurrentIndex(0);
            combo->setEnabled(false);
            return;
        }

        auto selectedIndex = combo->findData(toQString(mapping.targetLayerToken));
        if (selectedIndex < 0)
            selectedIndex = 0;

        combo->setCurrentIndex(selectedIndex);
        combo->setEnabled(true);
        mapping.targetLayerToken = toJuceString(combo->currentData().toString());
        mapping.targetLayerName = toJuceString(combo->currentText());
        mapping.targetParameterToken.clear();
        mapping.targetParameterName.clear();
        return;
    }

    if (isMappingContextAction(mapping.actionType))
    {
        mapping.targetLayerToken.clear();
        mapping.targetLayerName.clear();
        combo->addItem("N/A", "");
        combo->setCurrentIndex(0);
        combo->setEnabled(false);
        return;
    }

    if (!isEffectParmAction(mapping.actionType))
    {
        mapping.targetLayerToken.clear();
        mapping.targetLayerName.clear();
        mapping.targetParameterToken.clear();
        mapping.targetParameterName.clear();
        combo->addItem("N/A", "");
        combo->setCurrentIndex(0);
        combo->setEnabled(false);
        return;
    }

    auto filters = getObsSourceFilters(mapping.targetUuid);
    for (auto& filter : filters)
    {
        auto layerToken = juce::String(effectParmObsFilterLayerPrefix.data()) + filter.uuid;
        combo->addItem(toQString(filter.name), toQString(layerToken));
    }

    if (combo->count() == 0)
    {
        combo->addItem("No filters", "");
        mapping.targetLayerToken.clear();
        mapping.targetLayerName.clear();
        mapping.targetParameterToken.clear();
        mapping.targetParameterName.clear();
        combo->setCurrentIndex(0);
        combo->setEnabled(false);
        return;
    }

    auto selectedIndex = combo->findData(toQString(mapping.targetLayerToken));
    if (selectedIndex < 0)
        selectedIndex = 0;

    combo->setCurrentIndex(selectedIndex);
    combo->setEnabled(true);
    mapping.targetLayerToken = toJuceString(combo->currentData().toString());
    mapping.targetLayerName = toJuceString(combo->currentText());
}

void populateParameterComboForMapping(QComboBox* combo, atk::MidiControlMapping& mapping)
{
    combo->clear();

    if (isMappingContextAction(mapping.actionType))
    {
        auto options = atk::getMidiControlMappingContextParameterOptions();
        for (auto& option : options)
            combo->addItem(toQString(option.displayName), toQString(option.targetToken));

        if (combo->count() == 0)
        {
            combo->addItem("No values", "");
            mapping.targetParameterToken.clear();
            mapping.targetParameterName.clear();
            combo->setCurrentIndex(0);
            combo->setEnabled(false);
            return;
        }

        auto selectedIndex = combo->findData(toQString(mapping.targetParameterToken));
        if (selectedIndex < 0)
            selectedIndex = 0;

        combo->setCurrentIndex(selectedIndex);
        mapping.targetParameterToken = toJuceString(combo->currentData().toString());
        mapping.targetParameterName = toJuceString(combo->currentText());
        combo->setEnabled(true);
        return;
    }

    if (!isEffectParmAction(mapping.actionType))
    {
        mapping.targetParameterToken.clear();
        mapping.targetParameterName.clear();
        combo->addItem("N/A", "");
        combo->setCurrentIndex(0);
        combo->setEnabled(false);
        return;
    }

    auto filterUuid = parseEffectParmObsFilterUuidFromLayerToken(mapping.targetLayerToken);
    auto parameters = getObsFilterParameterEntries(mapping.targetUuid, filterUuid);

    for (auto& parameter : parameters)
        combo->addItem(toQString(parameter.displayName), toQString(parameter.key));

    if (combo->count() == 0)
    {
        combo->addItem("No writable parameters", "");
        mapping.targetParameterToken.clear();
        mapping.targetParameterName.clear();
        combo->setCurrentIndex(0);
        combo->setEnabled(false);
        return;
    }

    if (mapping.targetParameterToken.isNotEmpty())
    {
        if (combo->findData(toQString(mapping.targetParameterToken)) < 0)
            mapping.targetParameterToken.clear();

        auto selectedIndex = combo->findData(toQString(mapping.targetParameterToken));
        if (selectedIndex < 0)
            selectedIndex = 0;

        combo->setCurrentIndex(selectedIndex);
        mapping.targetParameterToken = toJuceString(combo->currentData().toString());
        mapping.targetParameterName = toJuceString(combo->currentText());
    }
    else
    {
        combo->setCurrentIndex(0);
        mapping.targetParameterToken = toJuceString(combo->currentData().toString());
        mapping.targetParameterName = toJuceString(combo->currentText());
    }

    combo->setEnabled(true);
}

void layoutMappingsTableColumns(QTableView* table)
{
    if (table == nullptr)
        return;

    constexpr std::array<int, midi_control_column_count> widths = {
        44, 72, 170, 170, 86, 64, 68, 96, 136, 160, 170, 190, 64, 64, 92, 70, 84, 84, 88,
    };

    for (int i = 0; i < midi_control_column_count; ++i)
        table->setColumnWidth(i, widths[size_t(i)]);
}

void openPersistentEditorsForSelectedRow(QTableView* table, QAbstractItemModel* model, int selectedRow, int previousRow)
{
    if (table == nullptr || model == nullptr)
        return;

    const std::array<int, 17> editableColumns = {
        midi_control_column_learn,
        midi_control_column_input_device,
        midi_control_column_output_route,
        midi_control_column_type,
        midi_control_column_channel,
        midi_control_column_data1,
        midi_control_column_mode,
        midi_control_column_action,
        midi_control_column_target,
        midi_control_column_layer,
        midi_control_column_parameter,
        midi_control_column_bank,
        midi_control_column_preset,
        midi_control_column_activation,
        midi_control_column_min,
        midi_control_column_max,
        midi_control_column_scale,
    };

    if (previousRow >= 0 && previousRow < model->rowCount())
    {
        for (int column : editableColumns)
        {
            auto index = model->index(previousRow, column);
            if (index.isValid())
                table->closePersistentEditor(index);
        }
    }

    if (selectedRow < 0 || selectedRow >= model->rowCount())
        return;

    for (int column : editableColumns)
    {
        auto index = model->index(selectedRow, column);
        if (index.isValid())
            table->openPersistentEditor(index);
    }
}

void normalizeDependentFields(atk::MidiControlMapping& mapping)
{
    auto sources = getObsSources();
    auto mediaSources = getObsSources(true);

    QComboBox targetCombo;
    populateTargetComboForMapping(&targetCombo, mapping, sources, mediaSources);

    QComboBox layerCombo;
    populateLayerComboForMapping(&layerCombo, mapping);

    QComboBox parameterCombo;
    populateParameterComboForMapping(&parameterCombo, mapping);
}

class MidiControlMappingsItemDelegate : public QStyledItemDelegate
{
public:
    explicit MidiControlMappingsItemDelegate(QObject* parent)
        : QStyledItemDelegate(parent)
    {
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override
    {
        if (!index.isValid())
            return nullptr;

        switch (index.column())
        {
        case midi_control_column_learn:
        {
            auto* button = new QToolButton(parent);
            button->setCheckable(true);
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            button->setFocusPolicy(Qt::NoFocus);
            button->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            return button;
        }
        case midi_control_column_input_device:
        case midi_control_column_output_route:
        case midi_control_column_type:
        case midi_control_column_mode:
        case midi_control_column_action:
        case midi_control_column_target:
        case midi_control_column_layer:
        case midi_control_column_parameter:
        case midi_control_column_activation:
            return new QComboBox(parent);
        case midi_control_column_channel:
        case midi_control_column_data1:
        case midi_control_column_bank:
        case midi_control_column_preset:
        {
            auto* spin = new QSpinBox(parent);

            if (index.column() == midi_control_column_channel)
                spin->setRange(0, 15);
            else if (index.column() == midi_control_column_data1)
                spin->setRange(0, 127);
            else
                spin->setRange(atk::midi_control_mapping_context_any, atk::midi_control_mapping_context_max);

            return spin;
        }
        case midi_control_column_min:
        case midi_control_column_max:
        case midi_control_column_scale:
        {
            auto* spin = new QDoubleSpinBox(parent);
            spin->setDecimals(6);
            spin->setRange(-1000000.0, 1000000.0);
            spin->setSingleStep(0.01);
            return spin;
        }
        default:
            return QStyledItemDelegate::createEditor(parent, QStyleOptionViewItem(), index);
        }
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        auto* tableModel =
            static_cast<atk::MidiControlMappingsTableModel*>(const_cast<QAbstractItemModel*>(index.model()));
        if (tableModel == nullptr || !index.isValid())
            return;

        atk::MidiControlMapping mapping;
        if (!tableModel->getMapping(index.row(), mapping))
            return;

        if (auto* combo = qobject_cast<QComboBox*>(editor))
        {
            combo->clear();
            combo->setEditable(false);

            switch (index.column())
            {
            case midi_control_column_input_device:
            {
                combo->addItem("Any", "");
                if (auto* controller = atk::MidiControlController::getInstanceWithoutCreating())
                {
                    auto inputDevices = controller->getAvailableInputDevices();
                    for (auto& device : inputDevices)
                        combo->addItem(toQString(device), toQString(device));
                }

                auto selected = combo->findData(toQString(mapping.inputDeviceName));
                if (selected < 0)
                    selected = 0;
                combo->setCurrentIndex(selected);
                break;
            }
            case midi_control_column_output_route:
            {
                combo->addItem("None", outputRouteNoneValue);
                combo->addItem("Any", outputRouteAnyValue);

                if (auto* controller = atk::MidiControlController::getInstanceWithoutCreating())
                {
                    auto outputDevices = controller->getAvailableOutputDevices();
                    for (auto& device : outputDevices)
                        combo->addItem(toQString(device), toQString(device));
                }

                QString selectedToken = outputRouteNoneValue;
                if (mapping.outputMode == atk::midi_control_output_mode_any)
                    selectedToken = outputRouteAnyValue;
                else if (mapping.outputMode == atk::midi_control_output_mode_specific)
                    selectedToken = toQString(mapping.outputDeviceName);

                auto selected = combo->findData(selectedToken);
                if (selected < 0)
                    selected = 0;
                combo->setCurrentIndex(selected);
                break;
            }
            case midi_control_column_type:
            {
                addMessageTypeOptions(combo);
                auto selected = combo->findData(mapping.messageType);
                if (selected < 0)
                    selected = 0;
                combo->setCurrentIndex(selected);
                break;
            }
            case midi_control_column_mode:
            {
                addModeOptions(combo);
                auto selected = combo->findData(mapping.interactionMode);
                if (selected < 0)
                    selected = 0;
                combo->setCurrentIndex(selected);
                break;
            }
            case midi_control_column_action:
            {
                addActionOptions(combo);
                auto selected = combo->findData(mapping.actionType);
                if (selected < 0)
                    selected = 0;
                combo->setCurrentIndex(selected);
                break;
            }
            case midi_control_column_target:
            {
                auto sources = getObsSources();
                auto mediaSources = getObsSources(true);
                populateTargetComboForMapping(combo, mapping, sources, mediaSources);
                break;
            }
            case midi_control_column_layer:
            {
                populateLayerComboForMapping(combo, mapping);
                break;
            }
            case midi_control_column_parameter:
            {
                populateParameterComboForMapping(combo, mapping);
                break;
            }
            case midi_control_column_activation:
            {
                addActivationOptions(combo);
                auto selected = combo->findData(mapping.activationMode);
                if (selected < 0)
                    selected = 0;
                combo->setCurrentIndex(selected);
                break;
            }
            default:
                break;
            }

            return;
        }

        if (auto* learnButton = qobject_cast<QToolButton*>(editor))
        {
            auto* controller = atk::MidiControlController::getInstanceWithoutCreating();
            auto learnState = controller != nullptr ? controller->getLearnState() : atk::MidiControlLearnState{};
            auto learningThisRow = learnState.active && mapping.mappingId == learnState.mappingId;
            learnButton->setChecked(learningThisRow);
            learnButton->setText(learningThisRow ? "Learning" : "Learn");
            return;
        }

        if (auto* spin = qobject_cast<QSpinBox*>(editor))
        {
            spin->setValue(index.data(Qt::EditRole).toInt());
            return;
        }

        if (auto* spin = qobject_cast<QDoubleSpinBox*>(editor))
        {
            spin->setValue(index.data(Qt::EditRole).toDouble());
            return;
        }

        QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override
    {
        auto* tableModel = static_cast<atk::MidiControlMappingsTableModel*>(model);
        if (tableModel == nullptr || !index.isValid())
            return;

        atk::MidiControlMapping mapping;
        if (!tableModel->getMapping(index.row(), mapping))
            return;

        if (auto* combo = qobject_cast<QComboBox*>(editor))
        {
            switch (index.column())
            {
            case midi_control_column_input_device:
            {
                auto value = toJuceString(combo->currentData().toString()).trim();
                mapping.inputDeviceName = value;
                break;
            }
            case midi_control_column_output_route:
            {
                auto route = toJuceString(combo->currentData().toString()).trim();
                if (route.equalsIgnoreCase(outputRouteNoneValue))
                {
                    mapping.outputMode = atk::midi_control_output_mode_none;
                    mapping.outputDeviceName.clear();
                }
                else if (route.equalsIgnoreCase(outputRouteAnyValue))
                {
                    mapping.outputMode = atk::midi_control_output_mode_any;
                    mapping.outputDeviceName.clear();
                }
                else
                {
                    mapping.outputMode = atk::midi_control_output_mode_specific;
                    mapping.outputDeviceName = route;
                }
                break;
            }
            case midi_control_column_type:
                mapping.messageType = combo->currentData().toInt();
                break;
            case midi_control_column_mode:
                mapping.interactionMode = combo->currentData().toInt();
                break;
            case midi_control_column_action:
            {
                auto& rowMapping = mapping;
                auto previousActionType = rowMapping.actionType;
                rowMapping.actionType = combo->currentData().toInt();

                if (rowMapping.actionType != previousActionType)
                {
                    rowMapping.targetUuid.clear();
                    rowMapping.targetName.clear();
                    rowMapping.targetLayerToken.clear();
                    rowMapping.targetLayerName.clear();
                    rowMapping.targetParameterToken.clear();
                    rowMapping.targetParameterName.clear();
                }

                if (isHotkeyAction(rowMapping.actionType))
                {
                    rowMapping.outputMode = atk::midi_control_output_mode_none;
                    rowMapping.outputDeviceName.clear();
                }

                normalizeDependentFields(rowMapping);
                break;
            }
            case midi_control_column_target:
            {
                syncMappingTargetFromCombo(mapping, combo);
                normalizeDependentFields(mapping);
                break;
            }
            case midi_control_column_layer:
            {
                mapping.targetLayerToken = toJuceString(combo->currentData().toString());
                mapping.targetLayerName = toJuceString(combo->currentText());
                mapping.targetParameterToken.clear();
                mapping.targetParameterName.clear();
                normalizeDependentFields(mapping);
                break;
            }
            case midi_control_column_parameter:
            {
                mapping.targetParameterToken = toJuceString(combo->currentData().toString());
                mapping.targetParameterName = toJuceString(combo->currentText());
                break;
            }
            case midi_control_column_activation:
                mapping.activationMode = combo->currentData().toInt();
                break;
            default:
                break;
            }

            tableModel->replaceMapping(index.row(), mapping);
            return;
        }

        if (auto* spin = qobject_cast<QSpinBox*>(editor))
        {
            model->setData(index, spin->value(), Qt::EditRole);
            return;
        }

        if (auto* spin = qobject_cast<QDoubleSpinBox*>(editor))
        {
            model->setData(index, spin->value(), Qt::EditRole);
            return;
        }

        QStyledItemDelegate::setModelData(editor, model, index);
    }
};

atk::MidiControlDialog*& getMidiControlDialogInstance()
{
    static atk::MidiControlDialog* instance = nullptr;
    return instance;
}
} // namespace

namespace atk
{

MidiControlDialog::MidiControlDialog(QWidget* parent)
    : QDialog(parent)
{
    getMidiControlDialogInstance() = this;

    setWindowTitle("atkAudio MIDI Control");
    resize(1320, 700);

    buildLayout();

    if (auto* controller = MidiControlController::getInstance())
    {
        learnStateListenerId = controller->addLearnStateListener(
            [this](const MidiControlLearnState& learnState)
            {
                QMetaObject::invokeMethod(
                    this,
                    [this, learnState]() { handleLearnStateChanged(learnState); },
                    Qt::QueuedConnection
                );
            }
        );

        activeContextListenerId = controller->addActiveContextListener(
            [this](int bank, int preset)
            {
                QMetaObject::invokeMethod(
                    this,
                    [this, bank, preset]() { handleActiveContextChanged(bank, preset); },
                    Qt::QueuedConnection
                );
            }
        );
    }

    reloadFromController();
}

MidiControlDialog::~MidiControlDialog()
{
    shuttingDown = true;
    rebuildingUi = true;

    if (getMidiControlDialogInstance() == this)
        getMidiControlDialogInstance() = nullptr;

    if (mappingsTable != nullptr)
    {
        if (auto* selectionModel = mappingsTable->selectionModel())
            QObject::disconnect(selectionModel, nullptr, this, nullptr);

        QObject::disconnect(mappingsTable, nullptr, this, nullptr);
        mappingsTable->setModel(nullptr);
    }

    if (mappingsModel != nullptr)
        QObject::disconnect(mappingsModel, nullptr, this, nullptr);

    mappingsTable = nullptr;
    mappingsModel = nullptr;

    if (learnStateListenerId > 0)
        if (auto* controller = MidiControlController::getInstanceWithoutCreating())
            controller->removeLearnStateListener(learnStateListenerId);

    if (activeContextListenerId > 0)
        if (auto* controller = MidiControlController::getInstanceWithoutCreating())
            controller->removeActiveContextListener(activeContextListenerId);
}

void MidiControlDialog::refreshIfOpen()
{
    auto* instance = getMidiControlDialogInstance();
    if (instance != nullptr && !instance->shuttingDown)
        instance->reloadFromController();
}

void MidiControlDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    reloadFromController();
}

void MidiControlDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);

    if (event == nullptr)
        return;

    if (event->type() != QEvent::ActivationChange)
        return;

    if (!isActiveWindow())
        return;

    refreshSourceOptions();
}

void MidiControlDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    layoutMappingsTableColumns(mappingsTable);
}

void MidiControlDialog::keyPressEvent(QKeyEvent* event)
{
    if (event != nullptr && event->key() == Qt::Key_Escape)
    {
        if (cancelLearningIfActive())
        {
            event->accept();
            return;
        }
    }

    if (event != nullptr && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter))
    {
        event->accept();
        return;
    }

    QDialog::keyPressEvent(event);
}

bool MidiControlDialog::cancelLearningIfActive()
{
    auto* controller = MidiControlController::getInstanceWithoutCreating();
    if (controller == nullptr)
        return false;

    auto learnState = controller->getLearnState();
    if (!learnState.active)
        return false;

    controller->cancelLearning();
    return true;
}

void MidiControlDialog::buildLayout()
{
    mainLayout = new QVBoxLayout(this);

    auto* heading = new QLabel("<a href=\"https://atkaudio.com\">atkAudio</a> MIDI Control", this);
    heading->setTextFormat(Qt::RichText);
    heading->setTextInteractionFlags(Qt::TextBrowserInteraction);
    heading->setOpenExternalLinks(true);
    heading->setStyleSheet("font-weight: bold; font-size: 14px;");
    mainLayout->addWidget(heading);

    statusLabel = new QLabel("Select MIDI inputs/outputs and create mappings.", this);
    mainLayout->addWidget(statusLabel);

    auto* activeContextRow = new QHBoxLayout();
    activeContextLabel = new QLabel("Active Bank/Preset: unavailable", this);
    openActiveContextDockButton = new QPushButton("Open", this);

    activeContextRow->addWidget(activeContextLabel);
    activeContextRow->addWidget(openActiveContextDockButton);
    activeContextRow->addStretch(1);
    mainLayout->addLayout(activeContextRow);

    connect(
        openActiveContextDockButton,
        &QPushButton::clicked,
        this,
        [](bool) { MidiControlActiveContextDock::showOrCreateAndRaise(); }
    );

    auto* delayedOutputRow = new QHBoxLayout();
    parameterAutoSyncCheckBox = new QCheckBox("Disable parameter auto-sync", this);
    delayedFeedbackOutputCheckBox = new QCheckBox("Delayed MIDI feedback output", this);
    matchByNameCheckBox = new QCheckBox("Match by name", this);
    delayedFeedbackOutputIdleMsSpinBox = new QSpinBox(this);
    delayedFeedbackOutputIdleMsSpinBox->setRange(50, 5000);
    delayedFeedbackOutputIdleMsSpinBox->setSuffix(" ms");
    delayedFeedbackOutputIdleMsSpinBox->setSingleStep(50);
    delayedFeedbackOutputIdleMsSpinBox->setValue(400);

    delayedOutputRow->addWidget(matchByNameCheckBox);
    delayedOutputRow->addWidget(parameterAutoSyncCheckBox);
    delayedOutputRow->addWidget(delayedFeedbackOutputCheckBox);
    delayedOutputRow->addWidget(delayedFeedbackOutputIdleMsSpinBox);
    delayedOutputRow->addStretch(1);
    mainLayout->addLayout(delayedOutputRow);

    connect(
        parameterAutoSyncCheckBox,
        &QCheckBox::toggled,
        this,
        [this](bool disabled)
        {
            if (rebuildingUi)
                return;

            auto* controller = MidiControlController::getInstance();
            if (controller == nullptr)
                return;

            controller->setParameterAutoSyncEnabled(!disabled);
        }
    );

    connect(
        delayedFeedbackOutputCheckBox,
        &QCheckBox::toggled,
        this,
        [this](bool enabled)
        {
            if (rebuildingUi)
                return;

            auto* controller = MidiControlController::getInstance();
            if (controller == nullptr)
                return;

            controller->setDelayedFeedbackOutputEnabled(enabled);
        }
    );

    connect(
        matchByNameCheckBox,
        &QCheckBox::toggled,
        this,
        [this](bool enabled)
        {
            if (rebuildingUi)
                return;

            auto* controller = MidiControlController::getInstance();
            if (controller == nullptr)
                return;

            controller->setMatchByNameEnabled(enabled);
        }
    );

    connect(
        delayedFeedbackOutputIdleMsSpinBox,
        &QSpinBox::valueChanged,
        this,
        [this](int value)
        {
            if (rebuildingUi)
                return;

            auto* controller = MidiControlController::getInstance();
            if (controller == nullptr)
                return;

            controller->setDelayedFeedbackOutputIdleMs(value);
        }
    );

    auto* topSection = new QWidget(this);
    auto* topSectionLayout = new QVBoxLayout(topSection);
    topSectionLayout->setContentsMargins(0, 0, 0, 0);
    topSectionLayout->setSpacing(0);

    auto* subscriptionsRow = new QHBoxLayout();

    auto* inputSubscriptionColumn = new QVBoxLayout();
    auto* inputDeviceHeading = new QLabel("MIDI inputs", this);
    inputDeviceHeading->setStyleSheet("font-weight: bold;");
    inputSubscriptionColumn->addWidget(inputDeviceHeading);

    inputDeviceScrollArea = new QScrollArea(this);
    inputDeviceScrollArea->setWidgetResizable(true);
    inputDeviceContainer = new QWidget(inputDeviceScrollArea);
    inputDeviceLayout = new QVBoxLayout(inputDeviceContainer);
    inputDeviceLayout->setContentsMargins(4, 4, 4, 4);
    inputDeviceLayout->setSpacing(4);
    inputDeviceContainer->setLayout(inputDeviceLayout);
    inputDeviceScrollArea->setWidget(inputDeviceContainer);
    inputDeviceScrollArea->setMinimumHeight(60);
    inputSubscriptionColumn->addWidget(inputDeviceScrollArea);
    subscriptionsRow->addLayout(inputSubscriptionColumn);

    auto* outputSubscriptionColumn = new QVBoxLayout();
    auto* outputDeviceHeading = new QLabel("MIDI outputs", this);
    outputDeviceHeading->setStyleSheet("font-weight: bold;");
    outputSubscriptionColumn->addWidget(outputDeviceHeading);

    outputDeviceScrollArea = new QScrollArea(this);
    outputDeviceScrollArea->setWidgetResizable(true);
    outputDeviceContainer = new QWidget(outputDeviceScrollArea);
    outputDeviceLayout = new QVBoxLayout(outputDeviceContainer);
    outputDeviceLayout->setContentsMargins(4, 4, 4, 4);
    outputDeviceLayout->setSpacing(4);
    outputDeviceContainer->setLayout(outputDeviceLayout);
    outputDeviceScrollArea->setWidget(outputDeviceContainer);
    outputDeviceScrollArea->setMinimumHeight(60);
    outputSubscriptionColumn->addWidget(outputDeviceScrollArea);
    subscriptionsRow->addLayout(outputSubscriptionColumn);

    topSectionLayout->addLayout(subscriptionsRow);
    topSection->setLayout(topSectionLayout);

    auto* mappingsSection = new QWidget(this);
    auto* mappingsSectionLayout = new QVBoxLayout(mappingsSection);
    mappingsSectionLayout->setContentsMargins(0, 0, 0, 0);
    mappingsSectionLayout->setSpacing(4);

    auto* mappingHeading = new QLabel("Mappings", this);
    mappingHeading->setStyleSheet("font-weight: bold;");
    mappingsSectionLayout->addWidget(mappingHeading);

    mappingsTable = new QTableView(this);
    mappingsModel = new MidiControlMappingsTableModel(&dialogMappings, this);
    mappingsTable->setModel(mappingsModel);
    mappingsTable->setItemDelegate(new MidiControlMappingsItemDelegate(mappingsTable));
    mappingsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mappingsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mappingsTable->setEditTriggers(
        QAbstractItemView::SelectedClicked | QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
    );
    mappingsTable->setMouseTracking(false);
    mappingsTable->viewport()->setMouseTracking(false);
    mappingsTable->verticalHeader()->setVisible(false);
    mappingsTable->setCornerButtonEnabled(false);
    mappingsTable->horizontalHeader()->setHighlightSections(false);
    mappingsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    mappingsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mappingsTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    connect(
        mappingsModel,
        &QAbstractItemModel::dataChanged,
        this,
        [this](const QModelIndex&, const QModelIndex&, const QVector<int>&)
        {
            if (rebuildingUi)
                return;

            applyMappingsToController();
        }
    );

    mappingsSectionLayout->addWidget(mappingsTable, 1);
    layoutMappingsTableColumns(mappingsTable);
    openPersistentEditorsForSelectedRow(mappingsTable, mappingsModel, -1, -1);

    auto* subscriptionsAndMappingsSplitter = new QSplitter(Qt::Vertical, this);
    subscriptionsAndMappingsSplitter->addWidget(topSection);
    subscriptionsAndMappingsSplitter->addWidget(mappingsSection);
    subscriptionsAndMappingsSplitter->setChildrenCollapsible(false);
    subscriptionsAndMappingsSplitter->setStretchFactor(0, 0);
    subscriptionsAndMappingsSplitter->setStretchFactor(1, 1);
    subscriptionsAndMappingsSplitter->setSizes({220, 420});
    mainLayout->addWidget(subscriptionsAndMappingsSplitter, 1);

    connect(
        mappingsTable,
        &QTableView::clicked,
        this,
        [this](const QModelIndex& index)
        {
            if (!index.isValid())
                return;

            if (index.column() != midi_control_column_learn)
                return;

            if (!isValidRow(index.row()))
                return;

            auto* controller = MidiControlController::getInstanceWithoutCreating();
            if (controller == nullptr)
                return;

            auto learnState = controller->getLearnState();
            auto mappingId = dialogMappings[size_t(index.row())].mappingId;
            auto isLearningThisRow = learnState.active && learnState.mappingId == mappingId;

            if (isLearningThisRow)
            {
                controller->cancelLearning();
                return;
            }

            controller->beginLearning(mappingId);
        }
    );

    connect(
        mappingsTable->selectionModel(),
        &QItemSelectionModel::currentRowChanged,
        this,
        [this](const QModelIndex& current, const QModelIndex&)
        {
            auto row = current.isValid() ? current.row() : -1;
            openPersistentEditorsForSelectedRow(mappingsTable, mappingsModel, row, persistentEditorsRow);
            persistentEditorsRow = row;
        }
    );

    auto* deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), mappingsTable);
    deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, [this]() { removeSelectedMappings(); });

    auto* buttonRow = new QHBoxLayout();
    addButton = new QPushButton("Add Mapping", this);
    cloneButton = new QPushButton("Clone Selected", this);
    removeButton = new QPushButton("Remove Selected", this);
    saveMappingsButton = new QPushButton("Save Mappings", this);
    loadMappingsButton = new QPushButton("Load Mappings", this);
    showLastTouchedHelperButton = new QPushButton("Open Last Touched", this);
    closeButton = new QPushButton("Close", this);

    buttonRow->addWidget(addButton);
    buttonRow->addWidget(cloneButton);
    buttonRow->addWidget(removeButton);
    buttonRow->addWidget(saveMappingsButton);
    buttonRow->addWidget(loadMappingsButton);
    buttonRow->addWidget(showLastTouchedHelperButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton);
    mainLayout->addLayout(buttonRow);

    connect(addButton, &QPushButton::clicked, this, [this]() { addMapping(); });
    connect(cloneButton, &QPushButton::clicked, this, [this]() { cloneSelectedMapping(); });
    connect(removeButton, &QPushButton::clicked, this, [this]() { removeSelectedMappings(); });
    connect(saveMappingsButton, &QPushButton::clicked, this, [this]() { saveMappingsToFile(); });
    connect(loadMappingsButton, &QPushButton::clicked, this, [this]() { loadMappingsFromFile(); });
    connect(showLastTouchedHelperButton, &QPushButton::clicked, this, [this]() { showLastTouchedHelperDialog(); });
    connect(closeButton, &QPushButton::clicked, this, [this]() { close(); });

    auto allButtons = findChildren<QPushButton*>();
    for (auto* button : allButtons)
    {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
}

void MidiControlDialog::reloadFromController()
{
    if (shuttingDown)
        return;

    auto* controller = MidiControlController::getInstance();
    if (controller != nullptr)
    {
        dialogMappings = controller->getMappings();
        if (mappingsModel != nullptr)
        {
            auto learnState = controller->getLearnState();
            mappingsModel->setLearnState(learnState.active, learnState.mappingId);
        }

        if (parameterAutoSyncCheckBox != nullptr
            && matchByNameCheckBox != nullptr
            && delayedFeedbackOutputCheckBox != nullptr
            && delayedFeedbackOutputIdleMsSpinBox != nullptr)
        {
            QSignalBlocker autoSyncBlocker(parameterAutoSyncCheckBox);
            QSignalBlocker matchByNameBlocker(matchByNameCheckBox);
            QSignalBlocker enabledBlocker(delayedFeedbackOutputCheckBox);
            QSignalBlocker delayBlocker(delayedFeedbackOutputIdleMsSpinBox);

            parameterAutoSyncCheckBox->setChecked(!controller->isParameterAutoSyncEnabled());
            delayedFeedbackOutputCheckBox->setChecked(controller->isDelayedFeedbackOutputEnabled());
            matchByNameCheckBox->setChecked(controller->isMatchByNameEnabled());
            delayedFeedbackOutputIdleMsSpinBox->setValue(controller->getDelayedFeedbackOutputIdleMs());
        }
    }
    else
    {
        dialogMappings.clear();
        if (mappingsModel != nullptr)
            mappingsModel->setLearnState(false, {});
    }

    rebuildSubscriptionWidgets();
    rebuildMappingsTable();
    refreshActiveContextDisplay();
    MidiControlLastTouchedHelperDialog::refreshIfOpen();
}

void MidiControlDialog::refreshActiveContextDisplay()
{
    if (activeContextLabel == nullptr)
        return;

    auto* controller = MidiControlController::getInstanceWithoutCreating();
    activeContextLabel->setText(getActiveContextDisplayText(controller));
    MidiControlActiveContextDock::refreshIfOpen();
}

void MidiControlDialog::handleActiveContextChanged(int bank, int preset)
{
    if (activeContextLabel == nullptr)
        return;

    activeContextLabel->setText(
        QString::fromUtf8("Active Bank/Preset: ") + QString::number(bank) + "/" + QString::number(preset)
    );
    MidiControlActiveContextDock::refreshIfOpen();
}

void MidiControlDialog::rebuildSubscriptionWidgets()
{
    if (shuttingDown || inputDeviceLayout == nullptr || outputDeviceLayout == nullptr)
        return;

    while (auto* item = inputDeviceLayout->takeAt(0))
    {
        if (item->widget() != nullptr)
            item->widget()->deleteLater();

        delete item;
    }

    while (auto* item = outputDeviceLayout->takeAt(0))
    {
        if (item->widget() != nullptr)
            item->widget()->deleteLater();

        delete item;
    }

    auto* controller = MidiControlController::getInstance();
    if (controller == nullptr)
        return;

    auto subscriptions = controller->getSubscriptions();
    auto inputDevices = controller->getAvailableInputDevices();
    auto outputDevices = controller->getAvailableOutputDevices();

    for (auto& deviceName : inputDevices)
    {
        auto* checkBox = new QCheckBox(toQString(deviceName), inputDeviceContainer);
        checkBox->setChecked(subscriptions.subscribedInputDevices.contains(deviceName));
        connect(checkBox, &QCheckBox::toggled, this, [this](bool) { syncSubscriptionsToController(); });
        inputDeviceLayout->addWidget(checkBox);
    }

    for (auto& deviceName : outputDevices)
    {
        auto* checkBox = new QCheckBox(toQString(deviceName), outputDeviceContainer);
        checkBox->setChecked(subscriptions.subscribedOutputDevices.contains(deviceName));
        connect(checkBox, &QCheckBox::toggled, this, [this](bool) { syncSubscriptionsToController(); });
        outputDeviceLayout->addWidget(checkBox);
    }

    inputDeviceLayout->addStretch(1);
    outputDeviceLayout->addStretch(1);
}

bool MidiControlDialog::isValidRow(int row) const
{
    return row >= 0 && row < int(dialogMappings.size());
}

void MidiControlDialog::rebuildMappingsTable()
{
    if (shuttingDown || mappingsTable == nullptr)
        return;

    int selectedRow = -1;
    if (auto* selectionModel = mappingsTable->selectionModel())
    {
        auto rows = selectionModel->selectedRows();
        if (!rows.isEmpty())
            selectedRow = rows.first().row();
    }

    rebuildingUi = true;

    if (mappingsModel != nullptr)
        mappingsModel->refreshAll();

    if (selectedRow < 0 && mappingsModel != nullptr && mappingsModel->rowCount() > 0)
        selectedRow = 0;

    openPersistentEditorsForSelectedRow(mappingsTable, mappingsModel, selectedRow, persistentEditorsRow);
    persistentEditorsRow = selectedRow;

    rebuildingUi = false;

    if (selectedRow >= 0 && mappingsModel != nullptr && selectedRow < mappingsModel->rowCount())
        mappingsTable->selectRow(selectedRow);

    layoutMappingsTableColumns(mappingsTable);
}

void MidiControlDialog::syncSubscriptionsToController()
{
    if (rebuildingUi || shuttingDown || inputDeviceLayout == nullptr || outputDeviceLayout == nullptr)
        return;

    MidiClientState subscriptions;

    for (int i = 0; i < inputDeviceLayout->count(); ++i)
    {
        auto* item = inputDeviceLayout->itemAt(i);
        auto* checkBox = item != nullptr ? qobject_cast<QCheckBox*>(item->widget()) : nullptr;

        if (checkBox != nullptr && checkBox->isChecked())
            subscriptions.subscribedInputDevices.add(toJuceString(checkBox->text()));
    }

    for (int i = 0; i < outputDeviceLayout->count(); ++i)
    {
        auto* item = outputDeviceLayout->itemAt(i);
        auto* checkBox = item != nullptr ? qobject_cast<QCheckBox*>(item->widget()) : nullptr;

        if (checkBox != nullptr && checkBox->isChecked())
            subscriptions.subscribedOutputDevices.add(toJuceString(checkBox->text()));
    }

    if (auto* controller = MidiControlController::getInstance())
        controller->setSubscriptions(subscriptions);

    rebuildMappingsTable();
}

bool MidiControlDialog::applyMappingsToController()
{
    if (rebuildingUi || shuttingDown)
        return true;

    juce::String validationError;
    if (!validateMidiControlMappings(dialogMappings, validationError))
    {
        statusLabel->setText(QString::fromUtf8("Invalid mapping: ") + toQString(validationError));
        return false;
    }

    if (!validateStrictMediaTargets(dialogMappings, validationError))
    {
        statusLabel->setText(QString::fromUtf8("Invalid mapping: ") + toQString(validationError));
        return false;
    }

    auto* controller = MidiControlController::getInstance();
    if (controller == nullptr)
    {
        statusLabel->setText("Controller unavailable.");
        return false;
    }

    controller->setMappings(dialogMappings);
    MidiControlLastTouchedHelperDialog::refreshIfOpen();

    return true;
}

void MidiControlDialog::removeSelectedMappings()
{
    if (shuttingDown || mappingsTable == nullptr)
        return;

    auto* selectionModel = mappingsTable->selectionModel();
    if (selectionModel == nullptr)
        return;

    auto selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty())
        return;

    std::vector<int> rows;
    rows.reserve(size_t(selectedRows.size()));
    for (auto& selectedRow : selectedRows)
    {
        int row = selectedRow.row();
        if (isValidRow(row))
            rows.push_back(row);
    }

    if (rows.empty())
        return;

    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    auto* controller = MidiControlController::getInstanceWithoutCreating();
    if (controller != nullptr)
    {
        auto learnState = controller->getLearnState();
        if (learnState.active)
        {
            for (int row : rows)
            {
                if (!isValidRow(row))
                    continue;

                if (dialogMappings[size_t(row)].mappingId == learnState.mappingId)
                {
                    controller->cancelLearning();
                    break;
                }
            }
        }
    }

    if (mappingsModel != nullptr)
        mappingsModel->removeRowsSortedUnique(rows);

    applyMappingsToController();
    rebuildMappingsTable();

    if (!dialogMappings.empty())
    {
        int nextRow = rows.front();
        if (nextRow >= int(dialogMappings.size()))
            nextRow = int(dialogMappings.size()) - 1;

        if (nextRow >= 0 && mappingsModel != nullptr && nextRow < mappingsModel->rowCount())
            mappingsTable->selectRow(nextRow);
    }
}

void MidiControlDialog::cloneSelectedMapping()
{
    if (shuttingDown || mappingsTable == nullptr)
        return;

    auto* selectionModel = mappingsTable->selectionModel();
    if (selectionModel == nullptr)
        return;

    auto selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty())
        return;

    int selectedRow = selectedRows.first().row();
    if (!isValidRow(selectedRow))
        return;

    auto clonedMapping = dialogMappings[size_t(selectedRow)];
    clonedMapping.mappingId = createMidiControlMappingId();
    clonedMapping.enabled = false;

    int cloneRow = selectedRow + 1;
    if (mappingsModel != nullptr)
        mappingsModel->insertMapping(cloneRow, clonedMapping);

    applyMappingsToController();
    rebuildMappingsTable();

    if (cloneRow >= 0 && mappingsModel != nullptr && cloneRow < mappingsModel->rowCount())
        mappingsTable->selectRow(cloneRow);

    statusLabel->setText("Cloned mapping as disabled copy.");
}

void MidiControlDialog::addMapping()
{
    if (shuttingDown || mappingsTable == nullptr)
        return;

    MidiControlMapping mapping;
    mapping.mappingId = createMidiControlMappingId();

    auto sources = getObsSources();
    if (!sources.empty())
    {
        mapping.targetUuid = sources.front().uuid;
        mapping.targetName = sources.front().name;
    }

    if (mappingsModel != nullptr)
        mappingsModel->appendMapping(mapping);

    applyMappingsToController();
    rebuildMappingsTable();

    int newRow = int(dialogMappings.size()) - 1;
    if (newRow >= 0 && mappingsModel != nullptr && newRow < mappingsModel->rowCount())
        mappingsTable->selectRow(newRow);
}

void MidiControlDialog::refreshSourceOptions()
{
    if (shuttingDown)
        return;

    rebuildMappingsTable();
    MidiControlLastTouchedHelperDialog::refreshIfOpen();
}

void MidiControlDialog::saveMappingsToFile()
{
    if (shuttingDown)
        return;

    juce::String validationError;
    if (!validateMidiControlMappings(dialogMappings, validationError))
    {
        statusLabel->setText(QString::fromUtf8("Invalid mapping: ") + toQString(validationError));
        return;
    }

    auto initialFile = lastMappingsFile == juce::File() ? getDefaultMappingsFile() : lastMappingsFile;

    mappingsFileChooser =
        std::make_unique<juce::FileChooser>("Save MIDI Control mappings", initialFile, mappingsFilePattern);

    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    mappingsFileChooser->launchAsync(
        flags,
        [this](const juce::FileChooser& chooser)
        {
            const auto result = chooser.getResult();
            if (result == juce::File())
                return;

            auto fileText = serializeMidiControlMappingsFilePayload(dialogMappings);
            if (!result.replaceWithText(fileText, false, false, "\n"))
            {
                statusLabel->setText("Failed to save mappings file.");
                return;
            }

            lastMappingsFile = result;
            statusLabel->setText(
                QString::fromUtf8("Saved ")
                + QString::number(int(dialogMappings.size()))
                + " mapping(s) to "
                + toQString(result.getFileName())
            );
        }
    );
}

void MidiControlDialog::loadMappingsFromFile()
{
    if (shuttingDown)
        return;

    auto initialFile = lastMappingsFile == juce::File() ? getDefaultMappingsFile() : lastMappingsFile;

    mappingsFileChooser =
        std::make_unique<juce::FileChooser>("Load MIDI Control mappings", initialFile, mappingsFilePattern);

    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    mappingsFileChooser->launchAsync(
        flags,
        [this](const juce::FileChooser& chooser)
        {
            const auto result = chooser.getResult();
            if (result == juce::File())
                return;

            auto content = result.loadFileAsString();
            std::vector<MidiControlMapping> loadedMappings;
            juce::String loadError;

            if (!deserializeMidiControlMappingsFilePayload(content, loadedMappings, loadError))
            {
                statusLabel->setText(QString::fromUtf8("Failed to load mappings: ") + toQString(loadError));
                return;
            }

            cancelLearningIfActive();

            dialogMappings = std::move(loadedMappings);

            if (!applyMappingsToController())
                return;

            rebuildMappingsTable();
            lastMappingsFile = result;

            statusLabel->setText(
                QString::fromUtf8("Loaded ")
                + QString::number(int(dialogMappings.size()))
                + " mapping(s) from "
                + toQString(result.getFileName())
            );
        }
    );
}

void MidiControlDialog::showLastTouchedHelperDialog()
{
    if (shuttingDown)
        return;

    MidiControlLastTouchedHelperDialog::showOrCreateAndRaise();
}

void MidiControlDialog::handleLearnStateChanged(const MidiControlLearnState& learnState)
{
    if (shuttingDown)
        return;

    if (mappingsModel != nullptr)
        mappingsModel->setLearnState(learnState.active, learnState.mappingId);

    if (learnState.active)
    {
        statusLabel->setText("Learning MIDI input for selected mapping...");
        rebuildMappingsTable();
        return;
    }

    if (learnState.sequence == lastLearnSequence)
    {
        statusLabel->setText("Learning cancelled.");
        rebuildMappingsTable();
        return;
    }

    lastLearnSequence = learnState.sequence;

    for (auto& mapping : dialogMappings)
    {
        if (mapping.mappingId != learnState.mappingId)
            continue;

        mapping.inputDeviceName = learnState.event.inputDeviceName;

        if (learnState.event.message.isController())
        {
            mapping.messageType = midi_control_message_type_cc;
            mapping.data1 = learnState.event.message.getControllerNumber();
        }
        else if (learnState.event.message.isNoteOn())
        {
            mapping.messageType = midi_control_message_type_note;
            mapping.data1 = learnState.event.message.getNoteNumber();
        }
        else if (learnState.event.message.isProgramChange())
        {
            mapping.messageType = midi_control_message_type_program_change;
            mapping.data1 = learnState.event.message.getProgramChangeNumber();
        }

        mapping.channel = learnState.event.message.getChannel();
        break;
    }

    applyMappingsToController();
    statusLabel->setText(QString::fromUtf8("Learned from ") + toQString(learnState.event.inputDeviceName));
    rebuildMappingsTable();
}

} // namespace atk
