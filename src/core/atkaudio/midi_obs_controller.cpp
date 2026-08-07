#include "midi_obs_controller.h"

#include <atkaudio/GlobalSettings.h>
#include <atkaudio/Logging.h>

#include <obs-frontend-api.h>
#include <obs-audio-controls.h>
#include <obs-module.h>

#include <QAbstractSlider>
#include <QObject>
#include <QThread>

#include <cstdlib>
#include <cmath>

namespace atk
{

namespace
{
MidiObsController* g_midiObsController = nullptr;

constexpr double pickupThreshold = 0.001;
constexpr int tbarMaxPosition = 1023;
constexpr int tbarZeroThreshold = 1;
constexpr int tbarCompleteThreshold = tbarMaxPosition - 1;
constexpr auto tbarObjectName = "tBar";

constexpr auto previousSceneTargetName = "Previous Scene";
constexpr auto nextSceneTargetName = "Next Scene";

bool isSceneSource(obs_source_t* source)
{
    return source != nullptr && obs_scene_from_source(source) != nullptr;
}

bool hasActiveTbarState(const MidiObsTransitionState& transitionState)
{
    return transitionState.lastPosition > 0 || transitionState.releasePending || transitionState.lockedUntilZero;
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
        atk::logging::error("MidiObsController::TBarEvent", "Fatal: TBar synchronization attempted from non-UI thread");
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
    auto* self = static_cast<MidiObsController*>(privateData);
    if (self == nullptr)
        return;

    if (event == OBS_FRONTEND_EVENT_TBAR_VALUE_CHANGED)
        self->handleTbarValueChanged();
}
} // namespace

MidiObsController* MidiObsController::getInstance()
{
    if (g_midiObsController == nullptr)
        g_midiObsController = new MidiObsController();

    return g_midiObsController;
}

MidiObsController* MidiObsController::getInstanceWithoutCreating()
{
    return g_midiObsController;
}

MidiObsController::MidiObsController()
    : midiClient(65536)
{
}

MidiObsController::~MidiObsController()
{
    shutdown();
    g_midiObsController = nullptr;
}

void MidiObsController::initialize()
{
    if (initialized)
        return;

    mappings = atk::settings::getMidiObsMappings();
    subscriptions = atk::settings::getMidiObsInputSubscriptions();
    midiClient.setSubscriptions(subscriptions);
    volumeFader = obs_fader_create(OBS_FADER_LOG);

    if (volumeFader == nullptr)
        atk::logging::error("MidiObsController::initialize", "failed to create OBS log volume fader");

    obs_frontend_add_event_callback(onFrontendEvent, this);

    startTimerHz(60);
    initialized = true;
}

void MidiObsController::shutdown()
{
    if (!initialized)
        return;

    obs_frontend_remove_event_callback(onFrontendEvent, this);

    stopTimer();

    if (volumeFader != nullptr)
    {
        obs_fader_destroy(volumeFader);
        volumeFader = nullptr;
    }

    transitionStates.clear();

    initialized = false;
}

std::vector<MidiObsMapping> MidiObsController::getMappings() const
{
    return mappings;
}

void MidiObsController::setMappings(const std::vector<MidiObsMapping>& newMappings)
{
    mappings = newMappings;
    pickupStates.clear();
    transitionStates.clear();

    atk::settings::setMidiObsMappings(newMappings);
}

MidiClientState MidiObsController::getSubscriptions() const
{
    return subscriptions;
}

void MidiObsController::setSubscriptions(const MidiClientState& state)
{
    subscriptions = state;
    midiClient.setSubscriptions(state);
    atk::settings::setMidiObsInputSubscriptions(state);
}

juce::StringArray MidiObsController::getAvailableInputDevices() const
{
    if (auto* server = MidiServer::getInstanceWithoutCreating())
        return server->getAvailableMidiInputDevices();

    return {};
}

void MidiObsController::beginLearning(const juce::String& mappingId)
{
    learnState.active = true;
    learnState.mappingId = mappingId;
}

void MidiObsController::cancelLearning()
{
    learnState.active = false;
    learnState.mappingId.clear();
}

MidiObsLearnState MidiObsController::getLearnState() const
{
    return learnState;
}

void MidiObsController::timerCallback()
{
    if (!initialized)
        return;

    std::vector<MidiInputEvent> events;
    midiClient.getPendingMidiEvents(events, 4096, 48000.0);

    for (auto& event : events)
        processEvent(event);
}

void MidiObsController::handleTbarValueChanged()
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
                "MidiObsController::TBarEvent",
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

void MidiObsController::processEvent(const MidiInputEvent& event)
{
    handleLearningEvent(event);

    std::vector<MidiObsMapping> currentMappings = mappings;

    for (auto& mapping : currentMappings)
    {
        if (!mapping.enabled)
            continue;

        if (mapping.inputDeviceName.isNotEmpty() && mapping.inputDeviceName != event.inputDeviceName)
            continue;

        if (mapping.channel > 0 && mapping.channel != event.message.getChannel())
            continue;

        if (mapping.messageType == midi_obs_message_type_cc)
        {
            if (!event.message.isController() || event.message.getControllerNumber() != mapping.data1)
                continue;
        }
        else if (mapping.messageType == midi_obs_message_type_note)
        {
            if (!(event.message.isNoteOn() || event.message.isNoteOff())
                || event.message.getNoteNumber() != mapping.data1)
                continue;
        }
        else if (mapping.messageType == midi_obs_message_type_program_change)
        {
            if (!event.message.isProgramChange() || event.message.getProgramChangeNumber() != mapping.data1)
                continue;
        }

        dispatchMapping(mapping, event);
    }
}

void MidiObsController::handleLearningEvent(const MidiInputEvent& event)
{
    if (!learnState.active)
        return;

    if (!(event.message.isController() || event.message.isNoteOn() || event.message.isProgramChange()))
        return;

    learnState.event = event;
    learnState.sequence += 1;
    learnState.active = false;
}

void MidiObsController::dispatchMapping(const MidiObsMapping& mapping, const MidiInputEvent& event)
{
    if (mapping.actionType == midi_obs_action_type_transition)
    {
        auto transitionKey = mapping.mappingId.toStdString();
        auto& transitionState = transitionStates[transitionKey];

        if (!obs_frontend_preview_program_mode_active())
        {
            if (hasActiveTbarState(transitionState))
                obs_frontend_release_tbar();

            transitionState = MidiObsTransitionState{};
            return;
        }

        if (mapping.interactionMode == midi_obs_interaction_mode_absolute)
        {
            const double mappedValue = getAbsoluteValue(mapping, event.message);
            const double normalizedValue = juce::jlimit(0.0, 1.0, mappedValue);
            const int tbarPosition =
                juce::jlimit(0, tbarMaxPosition, int(std::lround(normalizedValue * double(tbarMaxPosition))));

            const bool atZero = tbarPosition <= tbarZeroThreshold;
            const bool atComplete = tbarPosition >= tbarCompleteThreshold;
            const int previousPosition = transitionState.lastPosition;

            if (!atComplete)
                transitionState.releasePending = false;

            if (transitionState.lockedUntilZero)
            {
                if (atZero)
                    transitionState = MidiObsTransitionState{};
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

                transitionState = MidiObsTransitionState{};
                return;
            }
            return;
        }

        transitionState = MidiObsTransitionState{};

        if (shouldTrigger(event.message, mapping.messageType))
            obs_frontend_preview_program_trigger_transition();

        return;
    }

    if (mapping.actionType == midi_obs_action_type_scene)
    {
        bool shouldActivate = true;

        if (mapping.interactionMode == midi_obs_interaction_mode_absolute)
            shouldActivate = getAbsoluteValue(mapping, event.message) >= 0.5;
        else if (!shouldTrigger(event.message, mapping.messageType))
            shouldActivate = false;

        if (!shouldActivate)
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

        return;
    }

    auto* source = findTargetSource(mapping);
    if (source == nullptr)
        return;

    if (mapping.actionType == midi_obs_action_type_source_volume)
    {
        auto deflection = float(getAbsoluteValue(mapping, event.message));

        auto pickupKey = mapping.mappingId.toStdString();
        auto currentTargetUuid = juce::String(obs_source_get_uuid(source));
        const float currentVolume = obs_source_get_volume(source);

        float currentSourceDeflection = currentVolume;
        if (volumeFader != nullptr)
        {
            obs_fader_set_mul(volumeFader, currentVolume);
            currentSourceDeflection = obs_fader_get_deflection(volumeFader);
        }

        bool shouldWriteVolume = true;
        auto& pickupState = pickupStates[pickupKey];

        if (pickupState.targetUuid != currentTargetUuid)
        {
            pickupState.targetUuid = currentTargetUuid;
            pickupState.armed = true;
            pickupState.hasSourceDeflection = false;
            pickupState.hasControlDeflection = false;
        }

        if (pickupState.hasSourceDeflection
            && std::abs(currentSourceDeflection - pickupState.lastSourceDeflection) > pickupThreshold)
        {
            pickupState.armed = true;
        }

        if (pickupState.armed && volumeFader != nullptr)
        {
            bool shouldDisarm = false;

            if (pickupThreshold > 0.0)
            {
                shouldDisarm = std::abs(deflection - currentSourceDeflection) <= pickupThreshold;
            }
            else if (pickupState.hasControlDeflection)
            {
                shouldDisarm = (pickupState.lastControlDeflection < currentSourceDeflection
                                && deflection >= currentSourceDeflection)
                            || (pickupState.lastControlDeflection > currentSourceDeflection
                                && deflection <= currentSourceDeflection);
            }
            else
            {
                shouldDisarm = deflection == currentSourceDeflection;
            }

            if (shouldDisarm)
                pickupState.armed = false;
            else
                shouldWriteVolume = false;
        }

        pickupState.lastSourceDeflection = currentSourceDeflection;
        pickupState.hasSourceDeflection = true;
        pickupState.lastControlDeflection = deflection;
        pickupState.hasControlDeflection = true;

        if (!shouldWriteVolume)
        {
            obs_source_release(source);
            return;
        }

        if (volumeFader != nullptr)
        {
            obs_fader_set_deflection(volumeFader, deflection);
            const float volume = obs_fader_get_mul(volumeFader);
            obs_source_set_volume(source, volume);
            pickupState.lastSourceDeflection = deflection;
            pickupState.hasSourceDeflection = true;
            pickupState.lastControlDeflection = deflection;
            pickupState.hasControlDeflection = true;
        }
        else
        {
            obs_source_set_volume(source, deflection);
        }

        obs_source_release(source);
        return;
    }

    if (mapping.interactionMode == midi_obs_interaction_mode_absolute
        && mapping.actionType != midi_obs_action_type_source_volume)
    {
        auto value = getAbsoluteValue(mapping, event.message) >= 0.5;

        if (mapping.actionType == midi_obs_action_type_source_mute)
            obs_source_set_muted(source, value);
        else if (mapping.actionType == midi_obs_action_type_source_enabled)
            obs_source_set_enabled(source, value);
        else if (mapping.actionType == midi_obs_action_type_media_play_pause)
            obs_source_media_play_pause(source, !value);

        obs_source_release(source);
        return;
    }

    if (!shouldTrigger(event.message, mapping.messageType))
    {
        obs_source_release(source);
        return;
    }

    bool toggleValue = false;
    if (mapping.interactionMode == midi_obs_interaction_mode_toggle)
    {
        auto key = mapping.mappingId.toStdString();
        toggleValue = !toggleStates[key];
        toggleStates[key] = toggleValue;
    }

    if (mapping.actionType == midi_obs_action_type_source_mute)
        if (mapping.interactionMode == midi_obs_interaction_mode_toggle)
            obs_source_set_muted(source, toggleValue);
        else
            obs_source_set_muted(source, true);
    else if (mapping.actionType == midi_obs_action_type_source_enabled)
        if (mapping.interactionMode == midi_obs_interaction_mode_toggle)
            obs_source_set_enabled(source, toggleValue);
        else
            obs_source_set_enabled(source, true);
    else if (mapping.actionType == midi_obs_action_type_media_play_pause)
        if (mapping.interactionMode == midi_obs_interaction_mode_toggle)
            obs_source_media_play_pause(source, !toggleValue);
        else
            obs_source_media_play_pause(source, false);
    else if (mapping.actionType == midi_obs_action_type_media_restart)
        obs_source_media_restart(source);
    else if (mapping.actionType == midi_obs_action_type_media_stop)
        obs_source_media_stop(source);

    obs_source_release(source);
}

obs_source_t* MidiObsController::findTargetSource(const MidiObsMapping& mapping)
{
    obs_source_t* source = nullptr;

    if (mapping.targetUuid.isNotEmpty())
        source = obs_get_source_by_uuid(mapping.targetUuid.toRawUTF8());

    if (source == nullptr && mapping.targetName.isNotEmpty())
        source = obs_get_source_by_name(mapping.targetName.toRawUTF8());

    return source;
}

double MidiObsController::getAbsoluteValue(const MidiObsMapping& mapping, const juce::MidiMessage& message) const
{
    double normalized = 0.0;

    if (message.isController())
        normalized = message.getControllerValue() / 127.0;
    else if (message.isNoteOn())
        normalized = message.getVelocity();
    else if (message.isProgramChange())
        normalized = message.getProgramChangeNumber() / 127.0;

    if (mapping.valueInverted)
        normalized = 1.0 - normalized;

    return mapping.minValue + ((mapping.maxValue - mapping.minValue) * normalized);
}

bool MidiObsController::shouldTrigger(const juce::MidiMessage& message, int messageType) const
{
    if (messageType == midi_obs_message_type_cc)
        return message.isController() && message.getControllerValue() > 63;

    if (messageType == midi_obs_message_type_note)
        return message.isNoteOn();

    if (messageType == midi_obs_message_type_program_change)
        return message.isProgramChange();

    return false;
}

} // namespace atk
