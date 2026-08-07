#pragma once

#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <atkaudio/midi_control/midi_control_mapping.h>

#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct obs_source;
typedef struct obs_source obs_source_t;

struct obs_fader;
typedef struct obs_fader obs_fader_t;

struct calldata;
typedef struct calldata calldata_t;

namespace atk
{

struct MidiControlLearnState
{
    bool active = false;
    juce::String mappingId;
    MidiInputEvent event;
    int sequence = 0;
};

struct MidiControlPickupState
{
    bool armed = true;
    juce::String targetUuid;
    int lastSourceStep = -1;
    uint64_t lastTouchSequence = 0;
};

struct MidiControlTransitionState
{
    bool lockedUntilZero = false;
    bool releasePending = false;
    int lastPosition = 0;
};

struct MidiControlFeedbackState
{
    bool hasLastSentValue = false;
    double lastSentValue = 0.0;
    double suppressUntilMs = 0.0;
    bool inactiveFeedbackCleared = false;
};

struct MidiControlSourceObserver
{
    juce::String sourceUuid;
    bool observeVolume = false;
    bool observeMute = false;
    bool observeMonitoring = false;
};

struct MidiControlPendingDelayedFeedback
{
    MidiControlMapping mapping;
    double normalizedValue = 0.0;
    bool inactiveClear = false;
};

class MidiControlController : private juce::Timer
{
public:
    static MidiControlController* getInstance();
    static MidiControlController* getInstanceWithoutCreating();

    ~MidiControlController();

    void initialize();
    void shutdown();

    std::vector<MidiControlMapping> getMappings() const;
    void setMappings(const std::vector<MidiControlMapping>& mappings);

    MidiClientState getSubscriptions() const;
    void setSubscriptions(const MidiClientState& state);

    juce::StringArray getAvailableInputDevices() const;
    juce::StringArray getAvailableOutputDevices() const;

    void beginLearning(const juce::String& mappingId);
    void cancelLearning();
    MidiControlLearnState getLearnState() const;
    int addLearnStateListener(std::function<void(const MidiControlLearnState&)> listener);
    void removeLearnStateListener(int listenerId);
    int addActiveContextListener(std::function<void(int, int)> listener);
    void removeActiveContextListener(int listenerId);
    void handleTbarValueChanged();
    void requestFeedbackRefresh();
    void restoreStateIfReady();
    bool isParameterAutoSyncEnabled() const;
    bool isDelayedFeedbackOutputEnabled() const;
    bool isMatchByNameEnabled() const;
    int getDelayedFeedbackOutputIdleMs() const;
    int getActiveBank() const;
    int getActivePreset() const;
    void setActiveBank(int bank);
    void setActivePreset(int preset);
    void setParameterAutoSyncEnabled(bool enabled);
    void setDelayedFeedbackOutputEnabled(bool enabled);
    void setMatchByNameEnabled(bool enabled);
    void setDelayedFeedbackOutputIdleMs(int delayMs);
    void stopRuntimeResources();
    void resumeRuntimeResources();

private:
    MidiControlController();

    bool isRuntimeActive() const;
    void resetRuntimeState();
    void destroyFaders();

    void timerCallback();
    void processEvent(const MidiInputEvent& event);
    void requestFeedbackSyncFromInput(const MidiInputEvent& event);
    void processFeedbackSyncIfDue();
    void processPendingFeedbackEvents(bool forceSend = false);
    void processDelayedFeedbackFlushIfDue();
    void flushDelayedFeedbackOutputNow();
    void processFeedbackForMapping(const MidiControlMapping& mapping, bool forceSend = false);
    void handleLearningEvent(const MidiInputEvent& event);
    void dispatchMapping(const MidiControlMapping& mapping, const MidiInputEvent& event);
    void dispatchTransitionMapping(const MidiControlMapping& mapping, const MidiInputEvent& event);
    void dispatchTransitionAbsoluteMapping(
        const MidiControlMapping& mapping,
        const MidiInputEvent& event,
        MidiControlTransitionState& transitionState
    );
    bool handleLastTouchedHoldTarget(const MidiControlMapping& mapping, const MidiInputEvent& event);
    void resetTransitionState(MidiControlTransitionState& transitionState, bool releaseTbarIfActive);
    void dispatchMappingContextSwitch(const MidiControlMapping& mapping, const MidiInputEvent& event);
    void dispatchSceneMapping(const MidiControlMapping& mapping, const MidiInputEvent& event);
    void dispatchHotkeyMapping(const MidiControlMapping& mapping, const MidiInputEvent& event);
    void dispatchLastTouchedMapping(const MidiControlMapping& mapping, const MidiInputEvent& event);
    void dispatchEffectParmMapping(const MidiControlMapping& mapping, const MidiInputEvent& event);
    void dispatchSourceMapping(const MidiControlMapping& mapping, const MidiInputEvent& event);
    void
    handleSourceVolumeMapping(const MidiControlMapping& mapping, const MidiInputEvent& event, obs_source_t* source);
    void
    handleSourceDiscreteMapping(const MidiControlMapping& mapping, const MidiInputEvent& event, obs_source_t* source);
    void touchFeedbackState(const MidiControlMapping& mapping);
    void rebuildSourceObservers();
    void clearSourceObservers();
    void enqueueAllMappingsForFeedback();
    void enqueueLastTouchedMappingsForFeedback();
    void enqueueEffectParmMappingsForFeedback();
    void enqueueMappingForFeedback(const juce::String& mappingId);
    bool isFeedbackAction(int actionType) const;
    bool isMappingEligibleForFeedbackInCurrentContext(const MidiControlMapping& mapping) const;
    bool isFeedbackRouteActive(const MidiControlMapping& mapping) const;
    void suppressFeedbackForMapping(const MidiControlMapping& mapping);
    void queueDelayedFeedback(const MidiControlMapping& mapping, double normalizedValue, bool inactiveClear);
    void clearDelayedFeedbackQueue();
    int clampDelayedFeedbackOutputIdleMs(int delayMs) const;
    void sendFeedbackMessage(const MidiControlMapping& mapping, double normalizedValue);
    void applyMappingAutoHeal(
        const MidiControlMapping& mapping,
        juce::String resolvedTargetUuid,
        juce::String resolvedTargetLayerToken = {}
    );
    double getObservedActionValue(const MidiControlMapping& mapping, obs_source_t* source);
    double toMidiNormalizedValue(const MidiControlMapping& mapping, double observedValue);
    juce::MidiMessage createFeedbackMidiMessage(const MidiControlMapping& mapping, double normalizedValue);
    obs_source_t* findTargetSource(const MidiControlMapping& mapping);
    double getAbsoluteValue(const MidiControlMapping& mapping, const juce::MidiMessage& message) const;
    bool getAbsoluteSwitchValue(const MidiControlMapping& mapping, const juce::MidiMessage& message) const;
    int getMappingContextSpecificity(const MidiControlMapping& mapping) const;
    bool hasSameInputSignature(const MidiControlMapping& left, const MidiControlMapping& right) const;
    bool isMappingWinnerForCurrentContext(const MidiControlMapping& mapping) const;
    bool isMappingInActiveContext(const MidiControlMapping& mapping) const;
    void applyMappingContextChange();
    bool shouldActivateFromEvent(const MidiControlMapping& mapping, const juce::MidiMessage& message) const;
    bool shouldTrigger(const juce::MidiMessage& message, int messageType) const;
    void notifyLearnStateChanged(const MidiControlLearnState& state);
    void notifyActiveContextChanged(int bank, int preset);

    static void onObservedSourceSignal(void* privateData, calldata_t*);

    MidiClient midiClient;
    obs_fader_t* volumeFader = nullptr;
    obs_fader_t* feedbackVolumeFader = nullptr;
    std::vector<MidiControlMapping> mappings;
    MidiClientState subscriptions;
    std::unordered_map<std::string, bool> toggleStates;
    std::unordered_map<std::string, MidiControlPickupState> pickupStates;
    std::unordered_map<std::string, MidiControlTransitionState> transitionStates;
    std::unordered_map<std::string, MidiControlFeedbackState> feedbackStates;
    std::unordered_map<std::string, MidiControlSourceObserver> sourceObservers;
    std::unordered_set<std::string> pendingFeedbackMappingIds;
    std::vector<MidiInputEvent> pendingMidiEventsScratch;
    std::unordered_set<std::string> pendingFeedbackScratch;
    std::vector<const MidiControlMapping*> activeFeedbackMappingsScratch;
    std::vector<std::pair<const MidiControlMapping*, int>> dispatchMappingsScratch;
    std::atomic<bool> feedbackRefreshRequested{false};
    std::atomic<bool> pluginLastTouchedFeedbackPending{false};
    std::atomic<bool> obsFilterLastTouchedFeedbackPending{false};
    int pluginLastTouchedListenerId = 0;
    int obsFilterLastTouchedListenerId = 0;
    bool obsFilterPollEnabled = false;
    bool initialStateRestorePending = false;
    bool parameterAutoSyncEnabled = true;
    bool matchByNameEnabled = true;
    bool feedbackSyncPending = false;
    double feedbackSyncDeadlineMs = 0.0;
    bool delayedFeedbackOutputEnabled = true;
    int delayedFeedbackOutputIdleMs = 400;
    double delayedFeedbackOutputIdleDeadlineMs = 0.0;
    bool sourceObserverRebuildPending = false;
    std::unordered_map<std::string, MidiControlPendingDelayedFeedback> delayedFeedbackByMappingId;
    int activeBank = midi_control_mapping_context_min;
    int activePreset = midi_control_mapping_context_min;
    MidiControlLearnState learnState;
    juce::CriticalSection learnStateListenersLock;
    int nextLearnStateListenerId = 1;
    std::vector<std::pair<int, std::function<void(const MidiControlLearnState&)>>> learnStateListeners;
    juce::CriticalSection activeContextListenersLock;
    int nextActiveContextListenerId = 1;
    std::vector<std::pair<int, std::function<void(int, int)>>> activeContextListeners;
    bool runtimeStopping = false;
    bool initialized = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiControlController)
};

} // namespace atk
