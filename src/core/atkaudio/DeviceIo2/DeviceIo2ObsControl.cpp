#include <atkaudio/obs_monitoring.h>

#include "DeviceIo2ObsControl.h"

#include <obs-module.h>

#include <atomic>

namespace atk::deviceIo2ObsControl
{
// Called from the OBS graphics thread and the JUCE message thread, so only libobs (which locks its own
// state) may be used here. The frontend API reads Qt widgets owned by the UI thread; OBS clears them on
// quit and on scene collection switches, which crashed tick() (atkAudio/PluginForObsRelease#64).
namespace
{
constexpr int DEFAULT_TRANSITION_DURATION_MS = 300;
std::atomic<int> cachedTransitionDurationMs = DEFAULT_TRANSITION_DURATION_MS;

struct SceneUuidSearch
{
    const char* targetUuid = nullptr;
    bool found = false;
};

bool sceneContainsSourceUuidRecursive(obs_scene_t* scene, const char* targetUuid);

bool sceneContainsSourceUuidItem(obs_scene_t* scene, obs_sceneitem_t* item, void* privateData)
{
    UNUSED_PARAMETER(scene);

    auto* search = (SceneUuidSearch*)privateData;
    if (search == nullptr || item == nullptr)
        return true;

    auto* itemSource = obs_sceneitem_get_source(item);
    if (itemSource == nullptr)
        return true;

    auto* itemUuid = obs_source_get_uuid(itemSource);
    if (itemUuid != nullptr && search->targetUuid != nullptr && strcmp(itemUuid, search->targetUuid) == 0)
    {
        search->found = true;
        return false;
    }

    auto* childScene = obs_scene_from_source(itemSource);
    if (childScene != nullptr && sceneContainsSourceUuidRecursive(childScene, search->targetUuid))
    {
        search->found = true;
        return false;
    }

    return true;
}

bool sceneContainsSourceUuidRecursive(obs_scene_t* scene, const char* targetUuid)
{
    if (scene == nullptr || targetUuid == nullptr || targetUuid[0] == '\0')
        return false;

    SceneUuidSearch search;
    search.targetUuid = targetUuid;
    obs_scene_enum_items(scene, sceneContainsSourceUuidItem, &search);
    return search.found;
}

bool enumSceneContainsSourceUuid(void* privateData, obs_source_t* sceneSource)
{
    auto* search = (SceneUuidSearch*)privateData;
    obs_scene_t* scene = obs_scene_from_source(sceneSource);
    if (scene != nullptr && sceneContainsSourceUuidRecursive(scene, search->targetUuid))
    {
        search->found = true;
        return false;
    }

    return true;
}

bool sourceUuidExistsInAnyScene(const char* targetUuid)
{
    SceneUuidSearch search;
    search.targetUuid = targetUuid;
    obs_enum_scenes(enumSceneContainsSourceUuid, &search);
    return search.found;
}

bool tryCurrentSceneContainsSourceUuid(obs_source_t* programTransition, const char* targetUuid, bool& contains)
{
    contains = false;

    obs_source_t* currentSceneSource = obs_transition_get_active_source(programTransition);
    if (currentSceneSource == nullptr)
        return false;

    bool resolved = false;
    obs_scene_t* currentScene = obs_scene_from_source(currentSceneSource);
    if (currentScene != nullptr)
    {
        contains = sceneContainsSourceUuidRecursive(currentScene, targetUuid);
        resolved = true;
    }

    obs_source_release(currentSceneSource);
    return resolved;
}

bool tryTransitionDesiredBypass(obs_source_t* transition, const char* parentUuid, bool& desiredBypass)
{
    constexpr float kTransitionEpsilon = 0.001f;
    bool resolved = false;
    float transitionTime = obs_transition_get_time(transition);
    bool transitionActive = transitionTime > kTransitionEpsilon && transitionTime < (1.0f - kTransitionEpsilon);
    if (transitionActive)
    {
        obs_source_t* sourceSceneSrc = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_A);
        obs_source_t* destSceneSrc = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B);

        bool sourceSceneResolved = false;
        bool destSceneResolved = false;
        bool inSourceScene = false;
        bool inDestScene = false;

        if (sourceSceneSrc != nullptr)
        {
            obs_scene_t* sourceScene = obs_scene_from_source(sourceSceneSrc);
            if (sourceScene != nullptr)
            {
                inSourceScene = sceneContainsSourceUuidRecursive(sourceScene, parentUuid);
                sourceSceneResolved = true;
            }
            obs_source_release(sourceSceneSrc);
        }

        if (destSceneSrc != nullptr)
        {
            obs_scene_t* destScene = obs_scene_from_source(destSceneSrc);
            if (destScene != nullptr)
            {
                inDestScene = sceneContainsSourceUuidRecursive(destScene, parentUuid);
                destSceneResolved = true;
            }
            obs_source_release(destSceneSrc);
        }

        if (sourceSceneResolved && destSceneResolved)
        {
            desiredBypass = !inDestScene;
            if (inSourceScene && inDestScene)
                desiredBypass = false;

            resolved = true;
        }
    }

    return resolved;
}

struct SourceLevelQueryContext
{
    const char* targetUuid = nullptr;
    bool found = false;
    float sourceVolume = 1.0f;
    bool sourceMuted = false;
};

bool enumSourceLevelByUuid(void* param, obs_source_t* src)
{
    auto* context = (SourceLevelQueryContext*)param;
    if (context == nullptr || src == nullptr)
        return true;

    const char* sourceUuid = obs_source_get_uuid(src);
    if (sourceUuid == nullptr || context->targetUuid == nullptr)
        return true;

    if (strcmp(sourceUuid, context->targetUuid) != 0)
        return true;

    bool obsMuted = obs_source_muted(src);
    bool monitorOnly = atk::obsMonitoring::isMonitorOnly(src);
    bool effectiveMuted = obsMuted || monitorOnly;

    context->sourceVolume = obs_source_get_volume(src);
    context->sourceMuted = effectiveMuted;
    context->found = true;
    return false;
}
} // namespace

bool resolveDesiredBypass(const std::string& parentSourceUuid, bool& desiredBypass)
{
    if (parentSourceUuid.empty())
        return false;

    const char* uuid = parentSourceUuid.c_str();

    if (!sourceUuidExistsInAnyScene(uuid))
    {
        desiredBypass = false;
        return true;
    }

    // The frontend keeps its current transition on program output channel 0.
    obs_source_t* programTransition = obs_get_output_source(0);
    if (programTransition == nullptr)
        return false;

    bool resolved = false;
    if (obs_source_get_type(programTransition) == OBS_SOURCE_TYPE_TRANSITION)
    {
        resolved = tryTransitionDesiredBypass(programTransition, uuid, desiredBypass);

        bool inCurrentScene = false;
        if (!resolved && tryCurrentSceneContainsSourceUuid(programTransition, uuid, inCurrentScene))
        {
            desiredBypass = !inCurrentScene;
            resolved = true;
        }
    }

    obs_source_release(programTransition);
    return resolved;
}

bool readSourceLevelState(const std::string& parentSourceUuid, SourceLevelState& state)
{
    state.sourceVolume = 1.0f;
    state.sourceMuted = false;

    if (parentSourceUuid.empty())
        return false;

    SourceLevelQueryContext context;
    context.targetUuid = parentSourceUuid.c_str();
    obs_enum_sources(enumSourceLevelByUuid, &context);

    if (!context.found)
        return false;

    state.sourceVolume = context.sourceVolume;
    state.sourceMuted = context.sourceMuted;
    return true;
}

void setTransitionDurationMs(int durationMs)
{
    cachedTransitionDurationMs.store(durationMs, std::memory_order_release);
}

double getCurrentTransitionFadeSeconds()
{
    return cachedTransitionDurationMs.load(std::memory_order_acquire) / 1000.0;
}
} // namespace atk::deviceIo2ObsControl
