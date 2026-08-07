#include "DeviceIo2ObsControl.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace atk::deviceIo2ObsControl
{
namespace
{
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

bool sourceUuidExistsInAnyScene(const char* targetUuid)
{
    if (targetUuid == nullptr || targetUuid[0] == '\0')
        return false;

    obs_frontend_source_list sceneList = {};
    obs_frontend_get_scenes(&sceneList);

    bool found = false;
    for (size_t i = 0; i < sceneList.sources.num; ++i)
    {
        obs_source_t* sceneSource = sceneList.sources.array[i];
        if (sceneSource == nullptr)
            continue;

        obs_scene_t* scene = obs_scene_from_source(sceneSource);
        if (scene != nullptr && sceneContainsSourceUuidRecursive(scene, targetUuid))
        {
            found = true;
            break;
        }
    }

    obs_frontend_source_list_free(&sceneList);
    return found;
}

bool tryCurrentSceneContainsSourceUuid(const char* targetUuid, bool& contains)
{
    contains = false;

    if (targetUuid == nullptr || targetUuid[0] == '\0')
        return false;

    obs_source_t* currentSceneSource = obs_frontend_get_current_scene();
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

bool tryTransitionDesiredBypass(const char* parentUuid, bool& desiredBypass)
{
    if (parentUuid == nullptr || parentUuid[0] == '\0')
        return false;

    constexpr float kTransitionEpsilon = 0.001f;
    obs_source_t* transition = obs_frontend_get_current_transition();
    if (transition == nullptr)
        return false;

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

    obs_source_release(transition);
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
    int monitoringType = (int)obs_source_get_monitoring_type(src);
    bool effectiveMuted = obsMuted || (monitoringType == OBS_MONITORING_TYPE_MONITOR_ONLY);

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

    if (tryTransitionDesiredBypass(uuid, desiredBypass))
        return true;

    bool inCurrentScene = false;
    if (tryCurrentSceneContainsSourceUuid(uuid, inCurrentScene))
    {
        desiredBypass = !inCurrentScene;
        return true;
    }

    return false;
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

double getCurrentTransitionFadeSeconds()
{
    int transitionDurationMs = obs_frontend_get_transition_duration();
    return transitionDurationMs / 1000.0;
}
} // namespace atk::deviceIo2ObsControl
