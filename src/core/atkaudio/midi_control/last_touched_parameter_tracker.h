#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace atk
{

struct LastTouchedParameterEntry
{
    juce::AudioProcessor* processor = nullptr;
    juce::AudioProcessorParameter* parameter = nullptr;
    int parameterIndex = -1;
    juce::String ownerSourceUuid;
    juce::String ownerFilterName;
    juce::String identity;
    uint64_t touchSequence = 0;
};

class LastTouchedParameterTracker
{
public:
    static LastTouchedParameterTracker& getInstance();

    void registerProcessor(
        juce::AudioProcessor& processor,
        juce::String ownerSourceUuid = {},
        juce::String ownerFilterName = {}
    );
    void unregisterProcessor(juce::AudioProcessor& processor);
    void clear();

    bool getParameterAtOffset(int offset, LastTouchedParameterEntry& entry) const;
    bool getParameterByIdentity(const juce::String& identity, LastTouchedParameterEntry& entry) const;
    bool getParameterForOwner(
        const juce::String& ownerSourceUuid,
        const juce::String& ownerFilterName,
        int parameterIndex,
        LastTouchedParameterEntry& entry
    ) const;
    std::vector<LastTouchedParameterEntry>
    getParametersForOwner(const juce::String& ownerSourceUuid, const juce::String& ownerFilterName) const;
    std::vector<LastTouchedParameterEntry> getRecentParametersSnapshot() const;
    uint64_t getLatestTouchSequence() const;
    int addTouchListener(std::function<void()> listener);
    void removeTouchListener(int listenerId);
    void setHistoryHoldEnabled(bool enabled);
    bool isHistoryHoldEnabled() const;
    bool toggleHistoryHoldEnabled();
    void setActiveCollectionId(const juce::String& collectionId);
    juce::String getActiveCollectionId() const;
    void setPersistenceSuspended(bool suspended);
    void beginInternalWrite(juce::AudioProcessorParameter& parameter);
    void endInternalWrite(juce::AudioProcessorParameter& parameter);

private:
    struct ParameterState
    {
        LastTouchedParameterEntry entry;
    };

    struct InternalWriteState
    {
        int depth = 0;
        int postWriteSuppressionBudget = 0;
        double suppressionDeadlineMs = 0.0;
    };

    class ProcessorRegistration;

    LastTouchedParameterTracker() = default;
    ~LastTouchedParameterTracker() = default;

    void noteParameterTouched(
        juce::AudioProcessor& processor,
        int parameterIndex,
        const juce::String& ownerSourceUuid,
        const juce::String& ownerFilterName
    );
    void updateProcessorParameterState(
        juce::AudioProcessor& processor,
        const juce::String& ownerSourceUuid,
        const juce::String& ownerFilterName
    );
    void ensurePersistenceLoaded();
    void persistHistoryLocked() const;
    void notifyTouchListeners();
    juce::String
    createIdentity(const juce::String& ownerSourceUuid, const juce::String& ownerFilterName, int parameterIndex) const;
    void removeProcessorEntries(juce::AudioProcessor& processor);

    mutable juce::CriticalSection trackerLock;
    std::unordered_map<juce::AudioProcessor*, std::unique_ptr<ProcessorRegistration>> registrations;
    std::unordered_map<std::string, ParameterState> parameterStateByIdentity;
    std::vector<LastTouchedParameterEntry> recentParameters;
    std::unordered_map<juce::AudioProcessorParameter*, InternalWriteState> internalWriteStateByParameter;
    juce::CriticalSection touchListenersLock;
    int nextTouchListenerId = 1;
    std::vector<std::pair<int, std::function<void()>>> touchListeners;
    uint64_t nextTouchSequence = 0;
    bool historyHoldEnabled = false;
    juce::String activeCollectionId = "default";
    bool persistenceSuspended = false;
    bool persistenceLoaded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LastTouchedParameterTracker)
};

} // namespace atk
