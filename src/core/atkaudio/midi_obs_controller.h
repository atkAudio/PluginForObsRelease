#pragma once

#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <atkaudio/midi_obs_mapping.h>

#include <juce_events/juce_events.h>

#include <unordered_map>

struct obs_source;
typedef struct obs_source obs_source_t;

struct obs_fader;
typedef struct obs_fader obs_fader_t;

namespace atk
{

struct MidiObsLearnState
{
    bool active = false;
    juce::String mappingId;
    MidiInputEvent event;
    int sequence = 0;
};

struct MidiObsPickupState
{
    bool armed = true;
    juce::String targetUuid;
    double lastSourceDeflection = 0.0;
    bool hasSourceDeflection = false;
    double lastControlDeflection = 0.0;
    bool hasControlDeflection = false;
};

struct MidiObsTransitionState
{
    bool lockedUntilZero = false;
    bool releasePending = false;
    int lastPosition = 0;
};

class MidiObsController : private juce::Timer
{
public:
    static MidiObsController* getInstance();
    static MidiObsController* getInstanceWithoutCreating();

    ~MidiObsController();

    void initialize();
    void shutdown();

    std::vector<MidiObsMapping> getMappings() const;
    void setMappings(const std::vector<MidiObsMapping>& mappings);

    MidiClientState getSubscriptions() const;
    void setSubscriptions(const MidiClientState& state);

    juce::StringArray getAvailableInputDevices() const;

    void beginLearning(const juce::String& mappingId);
    void cancelLearning();
    MidiObsLearnState getLearnState() const;
    void handleTbarValueChanged();

private:
    MidiObsController();

    void timerCallback();
    void processEvent(const MidiInputEvent& event);
    void handleLearningEvent(const MidiInputEvent& event);
    void dispatchMapping(const MidiObsMapping& mapping, const MidiInputEvent& event);
    obs_source_t* findTargetSource(const MidiObsMapping& mapping);
    double getAbsoluteValue(const MidiObsMapping& mapping, const juce::MidiMessage& message) const;
    bool shouldTrigger(const juce::MidiMessage& message, int messageType) const;

    MidiClient midiClient;
    obs_fader_t* volumeFader = nullptr;
    std::vector<MidiObsMapping> mappings;
    MidiClientState subscriptions;
    std::unordered_map<std::string, bool> toggleStates;
    std::unordered_map<std::string, MidiObsPickupState> pickupStates;
    std::unordered_map<std::string, MidiObsTransitionState> transitionStates;
    MidiObsLearnState learnState;
    bool initialized = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiObsController)
};

} // namespace atk
