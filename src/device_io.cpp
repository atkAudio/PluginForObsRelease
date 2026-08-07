#include "core/atkaudio/DeviceIo/DeviceIo.h"
#include <atomic>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <juce_dsp/juce_dsp.h>

#define FILTER_NAME "atkAudio DeviceIo"
#define FILTER_ID "atkaudio_device_io"

#define OPEN_DEVICE_SETTINGS "open_device_settings"
#define OPEN_DEVICE_TEXT "Open Device Settings"
#define CLOSE_DEVICE_SETTINGS "close_device_settings"
#define CLOSE_DEVICE_TEXT "Close Device Settings"
#define MIX_INPUT_TEXT "Mix Input"
#define S_MIX_INPUT "mix_input"

#define IG_ID "input_gain"
#define OG_ID "output_gain"
#define IG_NAME "Input Gain"
#define OG_NAME "Output Gain"
#define FOLLOW_ID "follow_source_volume"
#define FOLLOW_NAME "Follow Source Volume/Mute"
#define FOLLOW_SCENE_ID "follow_scene"
#define FOLLOW_SCENE_NAME "Follow Scene"
#define OUTPUT_DELAY_ID "output_delay"
#define OUTPUT_DELAY_NAME "Output Delay"

struct adio_data
{
    obs_source_t* context = nullptr;
    obs_data_t* settings = nullptr;

    int channels = 0;
    double sampleRate = 0.0;

    std::atomic_bool mixInput = false;
    std::atomic_bool followSourceVolume = false;
    std::atomic_bool followScene = true;
    std::atomic<float> inputGain = 1.0f;
    std::atomic<float> outputGain = 1.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGainSmooth{1.0f};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> inputGainSmooth{1.0f};
    double gainSmoothSampleRate = 0.0;
    std::atomic<float> outputDelay = 0.0f;
    std::atomic<double> fadeTimeSeconds = 0.5;
    std::atomic_bool shouldBypass = false;
    bool hasDesiredBypass = false;
    bool desiredBypass = true;

    std::unique_ptr<atk::DeviceIo> deviceIo;

    bool hasLoadedState = false;
};

static const char* devio_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text(FILTER_NAME);
}

static void devio_destroy(void* data)
{
    struct adio_data* adio = (struct adio_data*)data;

    delete adio;
}

static void load(void* data, obs_data_t* settings)
{
    auto* adio = (struct adio_data*)data;
    if (adio->hasLoadedState)
        return;
    adio->hasLoadedState = true;

    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    std::string stateStr = chunkData ? chunkData : "";
    adio->deviceIo->setState(stateStr);
}

static void devio_update(void* data, obs_data_t* s)
{
    struct adio_data* adio = (struct adio_data*)data;
    adio->settings = s;
    adio->channels = (int)audio_output_get_channels(obs_get_audio());

    adio->mixInput.store(obs_data_get_bool(s, S_MIX_INPUT), std::memory_order_release);
    adio->followSourceVolume.store(obs_data_get_bool(s, FOLLOW_ID), std::memory_order_release);
    adio->followScene.store(obs_data_get_bool(s, FOLLOW_SCENE_ID), std::memory_order_release);

    auto inputGain = (float)obs_data_get_double(s, IG_ID);
    inputGain = obs_db_to_mul(inputGain);
    adio->inputGain.store(inputGain, std::memory_order_release);

    auto outputDelay = (float)obs_data_get_double(s, OUTPUT_DELAY_ID);
    adio->outputDelay.store(outputDelay, std::memory_order_release);
    adio->deviceIo->setOutputDelay(outputDelay);

    // auto outputGain = (float)obs_data_get_double(s, OG_ID);
    // outputGain = obs_db_to_mul(outputGain);
    // adio->outputGain.store(outputGain, std::memory_order_release);
}

static void* devio_create(obs_data_t* settings, obs_source_t* filter)
{
    struct adio_data* adio = new adio_data();
    adio->context = filter;

    auto numChannels = (int)audio_output_get_channels(obs_get_audio());
    auto sampleRate = audio_output_get_sample_rate(obs_get_audio());

    adio->channels = numChannels;
    adio->sampleRate = sampleRate;

    adio->deviceIo = std::make_unique<atk::DeviceIo>();

    devio_update(adio, settings);

    // Load state from settings if present (OBS load callback may not be called for all source types)
    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    if (chunkData && strlen(chunkData) > 0)
    {
        std::string stateStr = chunkData;
        adio->deviceIo->setState(stateStr);
        adio->hasLoadedState = true;
    }

    return adio;
}

static void devio_defaults(obs_data_t* s)
{
    obs_data_set_default_bool(s, S_MIX_INPUT, false);
    obs_data_set_default_bool(s, FOLLOW_ID, false);
    obs_data_set_default_bool(s, FOLLOW_SCENE_ID, true);
    obs_data_set_default_double(s, IG_ID, 0.0);
    obs_data_set_default_double(s, OG_ID, 0.0);
    obs_data_set_default_double(s, OUTPUT_DELAY_ID, 0.0);
}

static bool open_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    obs_property_set_visible(obs_properties_get(props, OPEN_DEVICE_SETTINGS), false);
    obs_property_set_visible(obs_properties_get(props, CLOSE_DEVICE_SETTINGS), true);

    adio_data* adio = (adio_data*)data;
    adio->deviceIo->setVisible(true);

    return true;
}

static bool close_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    obs_property_set_visible(obs_properties_get(props, OPEN_DEVICE_SETTINGS), true);
    obs_property_set_visible(obs_properties_get(props, CLOSE_DEVICE_SETTINGS), false);

    adio_data* adio = (adio_data*)data;
    adio->deviceIo->setVisible(false);

    return true;
}

static obs_properties_t* devio_properties(void* data)
{
    obs_properties_t* props = obs_properties_create();

    obs_properties_add_button2(props, OPEN_DEVICE_SETTINGS, OPEN_DEVICE_TEXT, open_editor_button_clicked, data);
    obs_properties_add_button2(props, CLOSE_DEVICE_SETTINGS, CLOSE_DEVICE_TEXT, close_editor_button_clicked, data);

    bool open_settings_vis = true;
    bool close_settings_vis = false;

    obs_property_set_visible(obs_properties_get(props, OPEN_DEVICE_SETTINGS), open_settings_vis);
    obs_property_set_visible(obs_properties_get(props, CLOSE_DEVICE_SETTINGS), close_settings_vis);

    obs_properties_add_bool(props, S_MIX_INPUT, obs_module_text(MIX_INPUT_TEXT));

    std::string propText = FOLLOW_ID;
    std::string textLabel = FOLLOW_NAME;

    obs_properties_add_bool(props, propText.c_str(), textLabel.c_str());

    obs_properties_add_bool(props, FOLLOW_SCENE_ID, FOLLOW_SCENE_NAME);

    propText = IG_ID;
    textLabel = IG_NAME;
    obs_property_t* p = obs_properties_add_float_slider(props, propText.c_str(), textLabel.c_str(), -30.0, 30.0, 0.1);
    obs_property_float_set_suffix(p, " dB");

    propText = OG_ID;
    textLabel = OG_NAME;
    p = obs_properties_add_float_slider(props, propText.c_str(), textLabel.c_str(), -30.0, 30.0, 0.1);
    obs_property_float_set_suffix(p, " dB");

    propText = OUTPUT_DELAY_ID;
    textLabel = OUTPUT_DELAY_NAME;
    p = obs_properties_add_float_slider(props, propText.c_str(), textLabel.c_str(), 0.0, 10000.0, 0.1);
    obs_property_float_set_suffix(p, " ms");

    UNUSED_PARAMETER(data);
    return props;
}

static struct obs_audio_data* devio_filter(void* data, struct obs_audio_data* audio)
{
    struct adio_data* adio = (struct adio_data*)data;
    auto channels = adio->channels;
    auto frames = audio->frames;
    float** adata = (float**)audio->data;

    auto outputGain = adio->outputGain.load(std::memory_order_acquire);
    auto inputGain = adio->inputGain.load(std::memory_order_acquire);

    if (adio->gainSmoothSampleRate != adio->sampleRate)
    {
        adio->outputGainSmooth.reset(adio->sampleRate, 0.05);
        adio->inputGainSmooth.reset(adio->sampleRate, 0.05);
        adio->outputGainSmooth.setCurrentAndTargetValue(outputGain);
        adio->inputGainSmooth.setCurrentAndTargetValue(inputGain);
        adio->gainSmoothSampleRate = adio->sampleRate;
    }

    adio->outputGainSmooth.setTargetValue(outputGain);
    adio->inputGainSmooth.setTargetValue(inputGain);

    for (size_t j = 0; j < frames; j++)
    {
        auto smoothGain = adio->outputGainSmooth.getNextValue();
        for (int i = 0; i < channels; i++)
            adata[i][j] *= smoothGain;
    }

    if (adio->followScene.load(std::memory_order_acquire))
    {
        adio->deviceIo->setFadeTime(adio->fadeTimeSeconds.load(std::memory_order_acquire));
        adio->deviceIo->setBypass(adio->shouldBypass.load(std::memory_order_acquire));
    }
    else
    {
        adio->deviceIo->setBypass(false);
    }

    adio->deviceIo->setMixInput(adio->mixInput.load(std::memory_order_acquire));
    adio->deviceIo->process(adata, channels, frames, adio->sampleRate);

    for (size_t j = 0; j < frames; j++)
    {
        auto smoothGain = adio->inputGainSmooth.getNextValue();
        for (int i = 0; i < channels; i++)
            adata[i][j] *= smoothGain;
    }

    return audio;
}

static void save(void* data, obs_data_t* settings)
{
    auto* adio = (struct adio_data*)data;
    std::string s;
    adio->deviceIo->getState(s);

    obs_data_set_string(settings, FILTER_ID, s.c_str());
}

struct scene_uuid_search
{
    const char* targetUuid = nullptr;
    bool found = false;
};

static bool scene_contains_source_uuid_recursive(obs_scene_t* scene, const char* targetUuid);

static bool scene_contains_source_uuid_item(obs_scene_t* scene, obs_sceneitem_t* item, void* privateData)
{
    UNUSED_PARAMETER(scene);

    auto* search = (scene_uuid_search*)privateData;
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
    if (childScene != nullptr && scene_contains_source_uuid_recursive(childScene, search->targetUuid))
    {
        search->found = true;
        return false;
    }

    return true;
}

static bool scene_contains_source_uuid_recursive(obs_scene_t* scene, const char* targetUuid)
{
    if (scene == nullptr || targetUuid == nullptr || targetUuid[0] == '\0')
        return false;

    scene_uuid_search search;
    search.targetUuid = targetUuid;
    obs_scene_enum_items(scene, scene_contains_source_uuid_item, &search);
    return search.found;
}

static bool source_uuid_exists_in_any_scene(const char* targetUuid)
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
        if (scene != nullptr && scene_contains_source_uuid_recursive(scene, targetUuid))
        {
            found = true;
            break;
        }
    }

    obs_frontend_source_list_free(&sceneList);
    return found;
}

static bool try_current_scene_contains_source_uuid(const char* targetUuid, bool& contains)
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
        contains = scene_contains_source_uuid_recursive(currentScene, targetUuid);
        resolved = true;
    }

    obs_source_release(currentSceneSource);
    return resolved;
}

static bool try_transition_desired_bypass(obs_source_t* parent, bool& desiredBypass)
{
    const char* parentUuid = obs_source_get_uuid(parent);
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
                inSourceScene = scene_contains_source_uuid_recursive(sourceScene, parentUuid);
                sourceSceneResolved = true;
            }
            obs_source_release(sourceSceneSrc);
        }

        if (destSceneSrc != nullptr)
        {
            obs_scene_t* destScene = obs_scene_from_source(destSceneSrc);
            if (destScene != nullptr)
            {
                inDestScene = scene_contains_source_uuid_recursive(destScene, parentUuid);
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

static bool try_resolve_desired_bypass(obs_source_t* parent, bool& desiredBypass)
{
    const char* parentUuid = obs_source_get_uuid(parent);
    if (parentUuid == nullptr || parentUuid[0] == '\0')
        return false;

    if (!source_uuid_exists_in_any_scene(parentUuid))
    {
        desiredBypass = false;
        return true;
    }

    if (try_transition_desired_bypass(parent, desiredBypass))
        return true;

    bool inCurrentScene = false;
    if (try_current_scene_contains_source_uuid(parentUuid, inCurrentScene))
    {
        desiredBypass = !inCurrentScene;
        return true;
    }

    return false;
}

static void tick(void* data, float seconds)
{
    struct adio_data* adio = (struct adio_data*)data;
    auto* settings = adio->settings;

    // Cache transition duration from frontend API (called on main thread)
    if (adio->followScene.load(std::memory_order_acquire))
    {
        int transitionDurationMs = obs_frontend_get_transition_duration();
        adio->fadeTimeSeconds.store(transitionDurationMs / 1000.0, std::memory_order_release);
    }

    // Compute bypass state on main thread for audio thread to read
    obs_source_t* parent = obs_filter_get_parent(adio->context);

    bool bypass = false;
    if (adio->followScene.load(std::memory_order_acquire) && parent)
    {
        bool desiredBypass = true;

        if (try_resolve_desired_bypass(parent, desiredBypass))
        {
            adio->desiredBypass = desiredBypass;
            adio->hasDesiredBypass = true;
        }

        bypass = adio->hasDesiredBypass ? adio->desiredBypass : true;
    }
    else
    {
        adio->hasDesiredBypass = false;
        adio->desiredBypass = true;
        bypass = false;
    }
    adio->shouldBypass.store(bypass, std::memory_order_release);

    auto outputGain = adio->outputGain.load(std::memory_order_acquire);
    if (settings)
        outputGain = (float)obs_data_get_double(settings, OG_ID);
    outputGain = obs_db_to_mul(outputGain);

    if (adio->followSourceVolume.load(std::memory_order_acquire) && parent)
    {
        bool obsMuted = obs_source_muted(parent);
        int monitoringType = (int)obs_source_get_monitoring_type(parent);
        bool effectiveMuted = obsMuted || (monitoringType == OBS_MONITORING_TYPE_MONITOR_ONLY);

        auto fader = obs_source_get_volume(parent);
        if (effectiveMuted)
            fader = 0.0f;
        outputGain *= fader;
    }

    adio->outputGain.store(outputGain, std::memory_order_release);

    UNUSED_PARAMETER(seconds);
}

struct obs_source_info device_io_filter = {
    .id = FILTER_ID,
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = devio_name,
    .create = devio_create,
    .destroy = devio_destroy,
    .get_defaults = devio_defaults,
    .get_properties = devio_properties,
    .update = devio_update,
    .video_tick = tick,
    .filter_audio = devio_filter,
    .save = save,
    .load = load,
};
