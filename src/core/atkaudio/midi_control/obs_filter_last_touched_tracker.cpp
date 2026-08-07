#include "obs_filter_last_touched_tracker.h"

#include <atkaudio/GlobalSettings.h>
#include "last_touched_parameter_tracker.h"
#include <atkaudio/Logging.h>

#include <obs-module.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace
{
constexpr int kMaxRecentParameters = 32;
constexpr double kTouchThreshold = 1.0e-6;
constexpr uint32_t kUiRefreshMinIntervalMs = 50;
constexpr const char* kDefaultHistoryCollectionId = "default";

[[noreturn]] void failFast(const char* context, const juce::String& message)
{
    atk::logging::error(context, "Fatal: " + message);
    std::abort();
}

juce::String normalizeCollectionId(const juce::String& collectionId)
{
    auto normalized = collectionId.trim();
    if (normalized.isEmpty())
        normalized = kDefaultHistoryCollectionId;

    return normalized;
}

uint32_t toObsFilterLaneFlags(uint32_t outputFlags)
{
    auto laneFlags = uint32_t(atk::obs_filter_last_touched_lane_none);
    if ((outputFlags & OBS_SOURCE_AUDIO) != 0)
        laneFlags |= atk::obs_filter_last_touched_lane_audio;
    if ((outputFlags & OBS_SOURCE_VIDEO) != 0)
        laneFlags |= atk::obs_filter_last_touched_lane_video;

    if (laneFlags == atk::obs_filter_last_touched_lane_none)
        failFast("ObsFilterLastTouchedTracker", "OBS filter has no audio/video output flags");

    return laneFlags;
}

struct UiRefreshState
{
    uint32_t lastRefreshMs = 0;
    bool pending = false;
    atk::ObsFilterLastTouchedEntry pendingEntry;
};

juce::CriticalSection g_uiRefreshLock;
std::unordered_map<std::string, UiRefreshState> g_uiRefreshStateByIdentity;

juce::CriticalSection g_filterSignalObserversLock;
std::unordered_map<std::string, obs_source_t*> g_filterSignalObservedSources;

void onObservedSourceFilterTopologyChanged(void* privateData, calldata_t* data)
{
    juce::ignoreUnused(data);
    auto* tracker = static_cast<atk::ObsFilterLastTouchedTracker*>(privateData);
    if (tracker != nullptr)
        tracker->markParameterKeyCacheDirty();
}

void disconnectFilterSignalObservers(atk::ObsFilterLastTouchedTracker* tracker)
{
    juce::ScopedLock lock(g_filterSignalObserversLock);

    for (auto& observedSource : g_filterSignalObservedSources)
    {
        auto* source = observedSource.second;
        if (source == nullptr)
            continue;

        auto* signalHandler = obs_source_get_signal_handler(source);
        if (signalHandler != nullptr)
        {
            signal_handler_disconnect(signalHandler, "filter_add", onObservedSourceFilterTopologyChanged, tracker);
            signal_handler_disconnect(signalHandler, "filter_remove", onObservedSourceFilterTopologyChanged, tracker);
        }

        obs_source_release(source);
    }

    g_filterSignalObservedSources.clear();
}

void reconnectFilterSignalObservers(
    const std::unordered_set<std::string>& sourceUuids,
    atk::ObsFilterLastTouchedTracker* tracker
)
{
    disconnectFilterSignalObservers(tracker);

    juce::ScopedLock lock(g_filterSignalObserversLock);

    for (const auto& sourceUuid : sourceUuids)
    {
        if (sourceUuid.empty())
            continue;

        auto* source = obs_get_source_by_uuid(sourceUuid.c_str());
        if (source == nullptr)
            continue;

        auto* signalHandler = obs_source_get_signal_handler(source);
        if (signalHandler == nullptr)
        {
            obs_source_release(source);
            continue;
        }

        signal_handler_connect(signalHandler, "filter_add", onObservedSourceFilterTopologyChanged, tracker);
        signal_handler_connect(signalHandler, "filter_remove", onObservedSourceFilterTopologyChanged, tracker);
        g_filterSignalObservedSources.emplace(sourceUuid, source);
    }
}

obs_source_t* findFilterByEntry(obs_source_t* source, atk::ObsFilterLastTouchedEntry& entry);

std::string getUiRefreshKey(const atk::ObsFilterLastTouchedEntry& entry)
{
    auto key = entry.identity.toStdString();
    if (key.empty())
        failFast("ObsFilterLastTouchedTracker", "OBS filter UI refresh requested with empty identity");

    return key;
}

bool refreshFilterPropertiesUi(const atk::ObsFilterLastTouchedEntry& entry)
{
    auto* source = obs_get_source_by_uuid(entry.sourceUuid.toRawUTF8());
    if (source == nullptr)
        return false;

    atk::ObsFilterLastTouchedEntry mutableEntry = entry;
    auto* targetFilter = findFilterByEntry(source, mutableEntry);
    obs_source_release(source);

    if (targetFilter == nullptr)
        return false;

    obs_source_update_properties(targetFilter);
    obs_source_release(targetFilter);
    return true;
}

void notifyUiRefreshed(const std::string& key, uint32_t nowMs)
{
    juce::ScopedLock lock(g_uiRefreshLock);
    auto& state = g_uiRefreshStateByIdentity[key];
    state.lastRefreshMs = nowMs;
    state.pending = false;
}

void requestFilterPropertiesUiRefresh(const atk::ObsFilterLastTouchedEntry& entry)
{
    auto key = getUiRefreshKey(entry);
    auto nowMs = juce::Time::getMillisecondCounter();

    {
        juce::ScopedLock lock(g_uiRefreshLock);
        auto& state = g_uiRefreshStateByIdentity[key];
        if ((nowMs - state.lastRefreshMs) < kUiRefreshMinIntervalMs)
        {
            state.pending = true;
            state.pendingEntry = entry;
            return;
        }
    }

    if (refreshFilterPropertiesUi(entry))
        notifyUiRefreshed(key, nowMs);
}

void flushPendingFilterPropertiesUiRefreshes()
{
    std::vector<std::pair<std::string, atk::ObsFilterLastTouchedEntry>> dueEntries;
    auto nowMs = juce::Time::getMillisecondCounter();

    {
        juce::ScopedLock lock(g_uiRefreshLock);

        for (auto& it : g_uiRefreshStateByIdentity)
        {
            auto& state = it.second;
            if (!state.pending)
                continue;

            if ((nowMs - state.lastRefreshMs) < kUiRefreshMinIntervalMs)
                continue;

            dueEntries.push_back({it.first, state.pendingEntry});
        }
    }

    for (auto& dueEntry : dueEntries)
        if (refreshFilterPropertiesUi(dueEntry.second))
            notifyUiRefreshed(dueEntry.first, nowMs);
}

struct FilterLookupContext
{
    juce::String wantedFilterUuid;
    obs_source_t* result = nullptr;
};

struct ReadValueContext
{
    atk::ObsFilterLastTouchedEntry entry;
    bool ok = false;
};

bool isWritableProperty(obs_property_t* property)
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

bool readNormalizedValue(obs_property_t* property, obs_data_t* settings, ReadValueContext& context)
{
    if (property == nullptr || settings == nullptr)
        return false;

    auto key = obs_property_name(property);
    if (key == nullptr || key[0] == '\0')
        return false;

    auto type = obs_property_get_type(property);

    context.entry.parameterKey = juce::String(key);
    context.entry.valueType = atk::obs_filter_last_touched_value_type_float;
    context.entry.normalizedValue = 0.0;
    context.ok = false;

    if (type == OBS_PROPERTY_BOOL)
    {
        context.entry.valueType = atk::obs_filter_last_touched_value_type_boolean;
        context.entry.normalizedValue = obs_data_get_bool(settings, key) ? 1.0 : 0.0;
        context.ok = true;
        return true;
    }

    if (type == OBS_PROPERTY_INT)
    {
        auto intMin = obs_property_int_min(property);
        auto intMax = obs_property_int_max(property);
        if (intMax <= intMin)
            return false;

        auto current = obs_data_get_int(settings, key);
        context.entry.valueType = atk::obs_filter_last_touched_value_type_integer;
        context.entry.normalizedValue = (double(current) - double(intMin)) / double(intMax - intMin);
        context.entry.normalizedValue = juce::jlimit(0.0, 1.0, context.entry.normalizedValue);
        context.ok = true;
        return true;
    }

    if (type == OBS_PROPERTY_FLOAT)
    {
        auto floatMin = obs_property_float_min(property);
        auto floatMax = obs_property_float_max(property);
        if (floatMax <= floatMin)
            return false;

        auto current = obs_data_get_double(settings, key);
        context.entry.valueType = atk::obs_filter_last_touched_value_type_float;
        context.entry.normalizedValue = (current - floatMin) / (floatMax - floatMin);
        context.entry.normalizedValue = juce::jlimit(0.0, 1.0, context.entry.normalizedValue);
        context.ok = true;
        return true;
    }

    if (type == OBS_PROPERTY_LIST)
    {
        auto itemCount = obs_property_list_item_count(property);
        if (itemCount <= 0)
            return false;

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

        context.entry.valueType = atk::obs_filter_last_touched_value_type_list_index;
        context.entry.normalizedValue = itemCount <= 1 ? 0.0 : double(selectedIndex) / double(itemCount - 1);
        context.entry.normalizedValue = juce::jlimit(0.0, 1.0, context.entry.normalizedValue);
        context.ok = true;
        return true;
    }

    return false;
}

obs_source_t* findFilterByEntry(obs_source_t* source, atk::ObsFilterLastTouchedEntry& entry)
{
    if (source == nullptr)
        return nullptr;

    FilterLookupContext context;
    context.wantedFilterUuid = entry.filterUuid;

    obs_source_enum_filters(
        source,
        [](obs_source_t* parent, obs_source_t* filter, void* privateData)
        {
            juce::ignoreUnused(parent);
            auto* context = static_cast<FilterLookupContext*>(privateData);
            if (context == nullptr || filter == nullptr || context->result != nullptr)
                return;

            auto filterUuid = juce::String(obs_source_get_uuid(filter));
            if (context->wantedFilterUuid.isNotEmpty() && context->wantedFilterUuid == filterUuid)
                context->result = obs_source_get_ref(filter);
        },
        &context
    );

    return context.result;
}

bool applyNormalizedValue(const atk::ObsFilterLastTouchedEntry& entry, double normalizedValue)
{
    auto* source = obs_get_source_by_uuid(entry.sourceUuid.toRawUTF8());
    if (source == nullptr)
        return false;

    atk::ObsFilterLastTouchedEntry mutableEntry = entry;
    auto* targetFilter = findFilterByEntry(source, mutableEntry);

    obs_source_release(source);

    if (targetFilter == nullptr)
        return false;

    auto* properties = obs_source_properties(targetFilter);
    auto* settings = obs_source_get_settings(targetFilter);

    if (properties == nullptr || settings == nullptr)
    {
        if (properties != nullptr)
            obs_properties_destroy(properties);
        if (settings != nullptr)
            obs_data_release(settings);
        obs_source_release(targetFilter);
        return false;
    }

    auto* property = obs_properties_get(properties, entry.parameterKey.toRawUTF8());
    if (property == nullptr)
    {
        obs_properties_destroy(properties);
        obs_data_release(settings);
        obs_source_release(targetFilter);
        return false;
    }

    auto value = juce::jlimit(0.0, 1.0, normalizedValue);

    auto key = entry.parameterKey.toRawUTF8();
    if (entry.valueType == atk::obs_filter_last_touched_value_type_boolean)
    {
        obs_data_set_bool(settings, key, value >= 0.5);
    }
    else if (entry.valueType == atk::obs_filter_last_touched_value_type_integer)
    {
        auto intMin = obs_property_int_min(property);
        auto intMax = obs_property_int_max(property);
        if (intMax <= intMin)
        {
            obs_properties_destroy(properties);
            obs_data_release(settings);
            obs_source_release(targetFilter);
            return false;
        }

        auto realValue = double(intMin) + value * double(intMax - intMin);
        obs_data_set_int(settings, key, int64_t(std::llround(realValue)));
    }
    else if (entry.valueType == atk::obs_filter_last_touched_value_type_float)
    {
        auto floatMin = obs_property_float_min(property);
        auto floatMax = obs_property_float_max(property);
        if (floatMax <= floatMin)
        {
            obs_properties_destroy(properties);
            obs_data_release(settings);
            obs_source_release(targetFilter);
            return false;
        }

        auto realValue = floatMin + value * (floatMax - floatMin);
        obs_data_set_double(settings, key, realValue);
    }
    else if (entry.valueType == atk::obs_filter_last_touched_value_type_list_index)
    {
        auto itemCount = obs_property_list_item_count(property);
        if (itemCount <= 0)
        {
            obs_properties_destroy(properties);
            obs_data_release(settings);
            obs_source_release(targetFilter);
            return false;
        }

        auto index = int(std::llround(value * double(itemCount - 1)));
        index = juce::jlimit(0, int(itemCount) - 1, index);

        auto format = obs_property_list_format(property);
        if (format == OBS_COMBO_FORMAT_INT)
            obs_data_set_int(settings, key, obs_property_list_item_int(property, size_t(index)));
        else if (format == OBS_COMBO_FORMAT_FLOAT)
            obs_data_set_double(settings, key, obs_property_list_item_float(property, size_t(index)));
        else
            obs_data_set_string(settings, key, obs_property_list_item_string(property, size_t(index)));
    }

    obs_source_update(targetFilter, settings);
    requestFilterPropertiesUiRefresh(entry);

    obs_properties_destroy(properties);
    obs_data_release(settings);
    obs_source_release(targetFilter);
    return true;
}

} // namespace

namespace atk
{

struct ObsFilterLastTouchedTracker::ParameterState
{
    ObsFilterLastTouchedEntry entry;
    double normalizedValue = 0.0;
    int suppressionBudget = 0;
};

ObsFilterLastTouchedTracker& ObsFilterLastTouchedTracker::getInstance()
{
    static ObsFilterLastTouchedTracker instance;
    return instance;
}

void ObsFilterLastTouchedTracker::clear()
{
    disconnectFilterSignalObservers(this);

    juce::ScopedLock lock(trackerLock);
    parameterStateByIdentity.clear();
    parameterKeyCache.clear();
    cachedKeysScratch.clear();
    touchedEntriesScratch.clear();
    currentIdentitiesScratch.clear();
    parameterKeyCacheDirty = true;
    recentParameters.clear();
    nextTouchSequence = 0;
    persistenceLoaded = true;
    persistHistoryLocked();

    juce::ScopedLock uiLock(g_uiRefreshLock);
    g_uiRefreshStateByIdentity.clear();
}

void ObsFilterLastTouchedTracker::suspend()
{
    disconnectFilterSignalObservers(this);

    juce::ScopedLock lock(trackerLock);
    parameterStateByIdentity.clear();
    parameterKeyCache.clear();
    cachedKeysScratch.clear();
    touchedEntriesScratch.clear();
    currentIdentitiesScratch.clear();
    parameterKeyCacheDirty = true;

    juce::ScopedLock uiLock(g_uiRefreshLock);
    g_uiRefreshStateByIdentity.clear();
}

void ObsFilterLastTouchedTracker::markParameterKeyCacheDirty()
{
    juce::ScopedLock lock(trackerLock);
    parameterKeyCacheDirty = true;
}

void ObsFilterLastTouchedTracker::rebuildParameterKeyCache()
{
    std::vector<CachedParameterKey> rebuiltKeys;
    std::unordered_set<std::string> sourceUuids;

    struct CacheBuildContext
    {
        std::vector<CachedParameterKey>* keys = nullptr;
        std::unordered_set<std::string>* sourceUuids = nullptr;
    } context;

    context.keys = &rebuiltKeys;
    context.sourceUuids = &sourceUuids;

    obs_enum_sources(
        [](void* context, obs_source_t* source)
        {
            auto* state = static_cast<CacheBuildContext*>(context);
            if (state == nullptr || state->keys == nullptr || state->sourceUuids == nullptr || source == nullptr)
                return true;

            auto sourceUuid = juce::String(obs_source_get_uuid(source));
            if (sourceUuid.isEmpty())
                return true;

            state->sourceUuids->insert(sourceUuid.toStdString());

            struct FilterEnumContext
            {
                std::vector<CachedParameterKey>* keys = nullptr;
                juce::String sourceUuid;
            } filterContext;

            filterContext.keys = state->keys;
            filterContext.sourceUuid = sourceUuid;

            obs_source_enum_filters(
                source,
                [](obs_source_t* parent, obs_source_t* filter, void* privateData)
                {
                    juce::ignoreUnused(parent);
                    auto* context = static_cast<FilterEnumContext*>(privateData);
                    if (context == nullptr || context->keys == nullptr || filter == nullptr)
                        return;

                    auto* properties = obs_source_properties(filter);
                    if (properties == nullptr)
                        return;

                    auto filterUuid = juce::String(obs_source_get_uuid(filter));
                    if (filterUuid.isEmpty())
                    {
                        obs_properties_destroy(properties);
                        return;
                    }

                    auto laneFlags = toObsFilterLaneFlags(obs_source_get_output_flags(filter));

                    auto* property = obs_properties_first(properties);
                    while (property != nullptr)
                    {
                        if (isWritableProperty(property))
                        {
                            auto key = obs_property_name(property);
                            if (key != nullptr && key[0] != '\0')
                            {
                                CachedParameterKey cachedKey;
                                cachedKey.sourceUuid = context->sourceUuid;
                                cachedKey.filterUuid = filterUuid;
                                cachedKey.parameterKey = juce::String(key);
                                cachedKey.identity =
                                    cachedKey.sourceUuid + "::" + cachedKey.filterUuid + "::" + cachedKey.parameterKey;
                                cachedKey.laneFlags = laneFlags;
                                context->keys->push_back(cachedKey);
                            }
                        }

                        obs_property_next(&property);
                    }

                    obs_properties_destroy(properties);
                },
                &filterContext
            );

            return true;
        },
        &context
    );

    reconnectFilterSignalObservers(sourceUuids, this);

    juce::ScopedLock lock(trackerLock);
    parameterKeyCache = std::move(rebuiltKeys);
    parameterKeyCacheDirty = false;
}

void ObsFilterLastTouchedTracker::poll()
{
    auto rebuildCache = false;

    {
        juce::ScopedLock lock(trackerLock);
        ensurePersistenceLoaded();
        rebuildCache = parameterKeyCacheDirty;
        cachedKeysScratch.clear();
        if (!rebuildCache)
            cachedKeysScratch = parameterKeyCache;
    }

    if (rebuildCache)
    {
        rebuildParameterKeyCache();

        juce::ScopedLock lock(trackerLock);
        cachedKeysScratch.clear();
        cachedKeysScratch = parameterKeyCache;
    }

    touchedEntriesScratch.clear();
    touchedEntriesScratch.reserve(cachedKeysScratch.size());

    for (auto& cachedKey : cachedKeysScratch)
    {
        if (cachedKey.identity.isEmpty())
            continue;

        auto* source = obs_get_source_by_uuid(cachedKey.sourceUuid.toRawUTF8());
        if (source == nullptr)
            continue;

        ObsFilterLastTouchedEntry lookupEntry;
        lookupEntry.sourceUuid = cachedKey.sourceUuid;
        lookupEntry.filterUuid = cachedKey.filterUuid;
        auto* targetFilter = findFilterByEntry(source, lookupEntry);
        obs_source_release(source);

        if (targetFilter == nullptr)
            continue;

        auto* properties = obs_source_properties(targetFilter);
        auto* settings = obs_source_get_settings(targetFilter);
        if (properties == nullptr || settings == nullptr)
        {
            if (properties != nullptr)
                obs_properties_destroy(properties);
            if (settings != nullptr)
                obs_data_release(settings);
            obs_source_release(targetFilter);
            continue;
        }

        auto* property = obs_properties_get(properties, cachedKey.parameterKey.toRawUTF8());
        if (property != nullptr && isWritableProperty(property))
        {
            ReadValueContext valueContext;
            if (readNormalizedValue(property, settings, valueContext) && valueContext.ok)
            {
                ObsFilterLastTouchedEntry entry;
                entry.sourceUuid = cachedKey.sourceUuid;
                entry.filterUuid = cachedKey.filterUuid;
                entry.parameterKey = cachedKey.parameterKey;
                entry.identity = cachedKey.identity;
                entry.valueType = valueContext.entry.valueType;
                entry.laneFlags = cachedKey.laneFlags;
                entry.normalizedValue = valueContext.entry.normalizedValue;
                touchedEntriesScratch.push_back(entry);
            }
        }

        obs_properties_destroy(properties);
        obs_data_release(settings);
        obs_source_release(targetFilter);
    }

    {
        juce::ScopedLock lock(trackerLock);
        ensurePersistenceLoaded();

        currentIdentitiesScratch.clear();
        currentIdentitiesScratch.reserve(touchedEntriesScratch.size());

        for (auto& candidate : touchedEntriesScratch)
        {
            auto key = candidate.identity.toStdString();
            currentIdentitiesScratch.insert(key);

            auto it = parameterStateByIdentity.find(key);
            if (it == parameterStateByIdentity.end())
            {
                ParameterState state;
                state.entry = candidate;
                state.normalizedValue = candidate.normalizedValue;
                parameterStateByIdentity.emplace(key, state);
                continue;
            }

            auto& state = it->second;

            if (state.suppressionBudget > 0)
            {
                state.normalizedValue = candidate.normalizedValue;
                state.entry = candidate;
                --state.suppressionBudget;
                continue;
            }

            if (std::abs(candidate.normalizedValue - state.normalizedValue) <= kTouchThreshold)
            {
                state.entry = candidate;
                continue;
            }

            state.normalizedValue = candidate.normalizedValue;
            state.entry = candidate;
            noteParameterTouched(candidate);
        }

        for (auto it = parameterStateByIdentity.begin(); it != parameterStateByIdentity.end();)
            if (currentIdentitiesScratch.find(it->first) == currentIdentitiesScratch.end())
                it = parameterStateByIdentity.erase(it);
            else
                ++it;

        auto hadStaleEntries = false;
        recentParameters.erase(
            std::remove_if(
                recentParameters.begin(),
                recentParameters.end(),
                [this, &hadStaleEntries](const ObsFilterLastTouchedEntry& entry)
                {
                    auto missing =
                        parameterStateByIdentity.find(entry.identity.toStdString()) == parameterStateByIdentity.end();
                    if (missing)
                        hadStaleEntries = true;

                    return missing;
                }
            ),
            recentParameters.end()
        );

        if (hadStaleEntries)
        {
            persistHistoryLocked();
            parameterKeyCacheDirty = true;
        }
    }

    flushPendingFilterPropertiesUiRefreshes();
}

bool ObsFilterLastTouchedTracker::getParameterAtOffset(int offset, ObsFilterLastTouchedEntry& entry) const
{
    juce::ScopedLock lock(trackerLock);

    const_cast<ObsFilterLastTouchedTracker*>(this)->ensurePersistenceLoaded();

    if (offset < 0 || offset >= int(recentParameters.size()))
        return false;

    auto& recentEntry = recentParameters[size_t(offset)];
    if (recentEntry.identity.isEmpty())
        failFast("ObsFilterLastTouchedTracker", "Recent OBS filter history entry has empty identity");

    auto stateIt = parameterStateByIdentity.find(recentEntry.identity.toStdString());
    if (stateIt == parameterStateByIdentity.end())
        return false;

    entry = stateIt->second.entry;
    entry.touchSequence = recentEntry.touchSequence;

    return true;
}

bool ObsFilterLastTouchedTracker::getAudioParameterAtOffset(int offset, ObsFilterLastTouchedEntry& entry) const
{
    return getParameterAtOffsetForLane(offset, obs_filter_last_touched_lane_audio, entry);
}

bool ObsFilterLastTouchedTracker::getVideoParameterAtOffset(int offset, ObsFilterLastTouchedEntry& entry) const
{
    return getParameterAtOffsetForLane(offset, obs_filter_last_touched_lane_video, entry);
}

bool ObsFilterLastTouchedTracker::getParameterByIdentity(
    const juce::String& identity,
    ObsFilterLastTouchedEntry& entry
) const
{
    juce::ScopedLock lock(trackerLock);

    auto key = identity.trim().toStdString();
    if (key.empty())
        return false;

    auto it = parameterStateByIdentity.find(key);
    if (it == parameterStateByIdentity.end())
        return false;

    entry = it->second.entry;
    entry.normalizedValue = it->second.normalizedValue;
    return true;
}

std::vector<ObsFilterLastTouchedEntry> ObsFilterLastTouchedTracker::getRecentParametersSnapshot() const
{
    juce::ScopedLock lock(trackerLock);

    const_cast<ObsFilterLastTouchedTracker*>(this)->ensurePersistenceLoaded();

    std::vector<ObsFilterLastTouchedEntry> snapshot;
    snapshot.reserve(recentParameters.size());

    for (const auto& candidate : recentParameters)
    {
        if (candidate.identity.isEmpty())
            failFast("ObsFilterLastTouchedTracker", "Recent OBS filter history entry has empty identity");

        auto stateIt = parameterStateByIdentity.find(candidate.identity.toStdString());
        if (stateIt == parameterStateByIdentity.end())
            continue;

        auto current = stateIt->second.entry;
        current.touchSequence = candidate.touchSequence;
        snapshot.push_back(current);
    }

    return snapshot;
}

uint64_t ObsFilterLastTouchedTracker::getLatestTouchSequence() const
{
    juce::ScopedLock lock(trackerLock);
    return nextTouchSequence;
}

int ObsFilterLastTouchedTracker::addTouchListener(std::function<void()> listener)
{
    if (!listener)
        return 0;

    const juce::ScopedLock lock(touchListenersLock);
    auto listenerId = nextTouchListenerId++;
    touchListeners.push_back({listenerId, std::move(listener)});
    return listenerId;
}

void ObsFilterLastTouchedTracker::removeTouchListener(int listenerId)
{
    if (listenerId <= 0)
        return;

    const juce::ScopedLock lock(touchListenersLock);

    touchListeners.erase(
        std::remove_if(
            touchListeners.begin(),
            touchListeners.end(),
            [listenerId](const std::pair<int, std::function<void()>>& entry) { return entry.first == listenerId; }
        ),
        touchListeners.end()
    );
}

void ObsFilterLastTouchedTracker::setActiveCollectionId(const juce::String& collectionId)
{
    juce::ScopedLock lock(trackerLock);

    auto normalized = normalizeCollectionId(collectionId);
    if (activeCollectionId == normalized && persistenceLoaded)
        return;

    activeCollectionId = normalized;
    recentParameters.clear();
    nextTouchSequence = 0;
    persistenceLoaded = false;
}

juce::String ObsFilterLastTouchedTracker::getActiveCollectionId() const
{
    juce::ScopedLock lock(trackerLock);
    return activeCollectionId;
}

void ObsFilterLastTouchedTracker::setPersistenceSuspended(bool suspended)
{
    juce::ScopedLock lock(trackerLock);
    persistenceSuspended = suspended;
}

bool ObsFilterLastTouchedTracker::setParameterNormalizedValue(ObsFilterLastTouchedEntry entry, double normalizedValue)
{
    {
        juce::ScopedLock lock(trackerLock);
        ensurePersistenceLoaded();
    }

    auto success = applyNormalizedValue(entry, normalizedValue);
    if (!success)
        return false;

    juce::ScopedLock lock(trackerLock);

    auto key = entry.identity.toStdString();
    auto it = parameterStateByIdentity.find(key);
    if (it != parameterStateByIdentity.end())
    {
        it->second.entry = entry;
        it->second.normalizedValue = juce::jlimit(0.0, 1.0, normalizedValue);
        it->second.suppressionBudget = 2;
    }

    return true;
}

void ObsFilterLastTouchedTracker::noteParameterTouched(ObsFilterLastTouchedEntry entry)
{
    auto shouldNotify = false;

    if (entry.identity.isEmpty())
        failFast("ObsFilterLastTouchedTracker", "Touched OBS filter entry has empty identity");
    if (entry.laneFlags == obs_filter_last_touched_lane_none)
        failFast("ObsFilterLastTouchedTracker", "Touched OBS filter entry has no audio/video lane");

    auto existing = std::find_if(
        recentParameters.begin(),
        recentParameters.end(),
        [&entry](ObsFilterLastTouchedEntry& candidate) { return candidate.identity == entry.identity; }
    );

    if (LastTouchedParameterTracker::getInstance().isHistoryHoldEnabled())
    {
        if (existing != recentParameters.end())
        {
            existing->touchSequence = ++nextTouchSequence;
            shouldNotify = true;
        }

        if (shouldNotify)
            notifyTouchListeners();
        return;
    }

    if (existing != recentParameters.end())
        recentParameters.erase(existing);

    entry.touchSequence = ++nextTouchSequence;
    recentParameters.insert(recentParameters.begin(), entry);

    if (int(recentParameters.size()) > kMaxRecentParameters)
        recentParameters.resize(size_t(kMaxRecentParameters));

    persistHistoryLocked();
    shouldNotify = true;

    if (shouldNotify)
        notifyTouchListeners();
}

void ObsFilterLastTouchedTracker::notifyTouchListeners()
{
    std::vector<std::function<void()>> listenersCopy;

    {
        const juce::ScopedLock lock(touchListenersLock);
        listenersCopy.reserve(touchListeners.size());
        for (const auto& entry : touchListeners)
            listenersCopy.push_back(entry.second);
    }

    for (auto& listener : listenersCopy)
        if (listener)
            listener();
}

void ObsFilterLastTouchedTracker::ensurePersistenceLoaded()
{
    if (persistenceLoaded)
        return;

    recentParameters.clear();
    nextTouchSequence = 0;

    auto persistedIdentities = settings::getObsFilterLastTouchedHistoryForCollection(activeCollectionId);
    for (auto& identity : persistedIdentities)
    {
        auto trimmed = identity.trim();
        if (trimmed.isEmpty())
            failFast("ObsFilterLastTouchedTracker", "Persisted OBS filter history contains empty identity");

        ObsFilterLastTouchedEntry entry;
        entry.identity = trimmed;
        entry.touchSequence = ++nextTouchSequence;
        recentParameters.push_back(entry);

        if (int(recentParameters.size()) >= kMaxRecentParameters)
            break;
    }

    persistenceLoaded = true;
}

bool ObsFilterLastTouchedTracker::getParameterAtOffsetForLane(
    int offset,
    uint32_t laneFlag,
    ObsFilterLastTouchedEntry& entry
) const
{
    juce::ScopedLock lock(trackerLock);

    const_cast<ObsFilterLastTouchedTracker*>(this)->ensurePersistenceLoaded();

    if (offset < 0)
        return false;

    auto laneOffset = 0;

    for (const auto& recentEntry : recentParameters)
    {
        if (recentEntry.identity.isEmpty())
            failFast("ObsFilterLastTouchedTracker", "Recent OBS filter history entry has empty identity");

        auto stateIt = parameterStateByIdentity.find(recentEntry.identity.toStdString());
        if (stateIt == parameterStateByIdentity.end())
            continue;

        auto current = stateIt->second.entry;
        if ((current.laneFlags & laneFlag) == 0)
            continue;

        if (laneOffset == offset)
        {
            entry = current;
            entry.touchSequence = recentEntry.touchSequence;
            return true;
        }

        ++laneOffset;
    }

    return false;
}

void ObsFilterLastTouchedTracker::persistHistoryLocked() const
{
    if (persistenceSuspended)
        return;

    std::vector<juce::String> identities;
    identities.reserve(recentParameters.size());

    for (auto& entry : recentParameters)
    {
        if (entry.identity.isEmpty())
            failFast("ObsFilterLastTouchedTracker", "Cannot persist OBS filter history with empty identity");

        identities.push_back(entry.identity);
    }

    settings::setObsFilterLastTouchedHistoryForCollection(activeCollectionId, identities);
}

} // namespace atk
