#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace atk
{

enum ObsFilterLastTouchedValueType
{
    obs_filter_last_touched_value_type_boolean = 0,
    obs_filter_last_touched_value_type_integer,
    obs_filter_last_touched_value_type_float,
    obs_filter_last_touched_value_type_list_index,
};

enum ObsFilterLastTouchedLaneFlags
{
    obs_filter_last_touched_lane_none = 0,
    obs_filter_last_touched_lane_audio = 1 << 0,
    obs_filter_last_touched_lane_video = 1 << 1,
};

struct ObsFilterLastTouchedEntry
{
    juce::String sourceUuid;
    juce::String filterUuid;
    juce::String parameterKey;
    juce::String identity;
    int valueType = obs_filter_last_touched_value_type_float;
    uint32_t laneFlags = obs_filter_last_touched_lane_none;
    double normalizedValue = 0.0;
    uint64_t touchSequence = 0;
};

class ObsFilterLastTouchedTracker
{
public:
    static ObsFilterLastTouchedTracker& getInstance();

    void clear();
    void suspend();
    void markParameterKeyCacheDirty();
    void poll();

    bool getParameterAtOffset(int offset, ObsFilterLastTouchedEntry& entry) const;
    bool getAudioParameterAtOffset(int offset, ObsFilterLastTouchedEntry& entry) const;
    bool getVideoParameterAtOffset(int offset, ObsFilterLastTouchedEntry& entry) const;
    bool getParameterByIdentity(const juce::String& identity, ObsFilterLastTouchedEntry& entry) const;
    std::vector<ObsFilterLastTouchedEntry> getRecentParametersSnapshot() const;
    uint64_t getLatestTouchSequence() const;
    int addTouchListener(std::function<void()> listener);
    void removeTouchListener(int listenerId);
    void setActiveCollectionId(const juce::String& collectionId);
    juce::String getActiveCollectionId() const;
    void setPersistenceSuspended(bool suspended);

    bool setParameterNormalizedValue(ObsFilterLastTouchedEntry entry, double normalizedValue);

private:
    struct ParameterState;

    struct CachedParameterKey
    {
        juce::String sourceUuid;
        juce::String filterUuid;
        juce::String parameterKey;
        juce::String identity;
        uint32_t laneFlags = obs_filter_last_touched_lane_none;
    };

    ObsFilterLastTouchedTracker() = default;
    ~ObsFilterLastTouchedTracker() = default;

    void rebuildParameterKeyCache();
    void noteParameterTouched(ObsFilterLastTouchedEntry entry);
    void notifyTouchListeners();
    bool getParameterAtOffsetForLane(int offset, uint32_t laneFlag, ObsFilterLastTouchedEntry& entry) const;
    void ensurePersistenceLoaded();
    void persistHistoryLocked();
    void schedulePersistHistoryLocked();

    mutable juce::CriticalSection trackerLock;
    std::unordered_map<std::string, ParameterState> parameterStateByIdentity;
    std::vector<CachedParameterKey> parameterKeyCache;
    std::vector<ObsFilterLastTouchedEntry> recentParameters;
    std::vector<CachedParameterKey> cachedKeysScratch;
    std::vector<ObsFilterLastTouchedEntry> touchedEntriesScratch;
    std::unordered_set<std::string> currentIdentitiesScratch;
    juce::CriticalSection touchListenersLock;
    int nextTouchListenerId = 1;
    std::vector<std::pair<int, std::function<void()>>> touchListeners;
    bool parameterKeyCacheDirty = true;
    uint64_t nextTouchSequence = 0;
    juce::String activeCollectionId = "default";
    bool persistenceSuspended = false;
    bool persistenceLoaded = false;
    std::atomic<bool> persistFlushPending{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ObsFilterLastTouchedTracker)
};

} // namespace atk
