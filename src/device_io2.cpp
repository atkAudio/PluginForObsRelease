#include "core/atkaudio/DeviceIo2/DeviceIo2.h"
#include "core/atkaudio/DeviceIo2/DeviceIo2ObsControl.h"
#include <atomic>
#include <obs-frontend-api.h>
#include <obs-module.h>

#define FILTER_NAME "atkAudio DeviceIo2"
#define FILTER_ID "atkaudio_device_io2"

#define OPEN_DEVICE_SETTINGS "open_device_settings"
#define OPEN_DEVICE_TEXT "Open Device Settings"
#define CLOSE_DEVICE_SETTINGS "close_device_settings"
#define CLOSE_DEVICE_TEXT "Close Device Settings"

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

struct adio2_data
{
    obs_source_t* context = nullptr;
    obs_data_t* settings = nullptr;

    int channels = 0;
    double sampleRate = 0.0;

    std::atomic_bool followSourceVolume = false;
    std::atomic_bool followScene = true;
    std::atomic<float> inputGainDb = 0.0f;
    std::atomic<float> outputGainDb = 0.0f;
    std::atomic<float> outputDelay = 0.0f;
    std::atomic<double> fadeTimeSeconds = 0.5;
    bool hasDesiredBypass = false;
    bool desiredBypass = true;
    std::atomic<float> sourceVolume = 1.0f;
    std::atomic_bool sourceMuted = false;

    atk::DeviceIo2 deviceIo2;

    bool hasLoadedState = false;
};

static const char* deviceio2_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text(FILTER_NAME);
}

static void deviceio2_destroy(void* data)
{
    struct adio2_data* adio = (struct adio2_data*)data;

    delete adio;
}

static void load(void* data, obs_data_t* settings)
{
    auto* adio = (struct adio2_data*)data;
    if (adio->hasLoadedState)
        return;
    adio->hasLoadedState = true;

    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    std::string stateStr = chunkData ? chunkData : "";
    adio->deviceIo2.setState(stateStr);
}

static void deviceio2_update(void* data, obs_data_t* s)
{
    struct adio2_data* adio = (struct adio2_data*)data;
    adio->settings = s;
    adio->channels = (int)audio_output_get_channels(obs_get_audio());

    adio->followSourceVolume.store(obs_data_get_bool(s, FOLLOW_ID), std::memory_order_release);
    adio->followScene.store(obs_data_get_bool(s, FOLLOW_SCENE_ID), std::memory_order_release);

    auto inputGain = (float)obs_data_get_double(s, IG_ID);
    adio->inputGainDb.store(inputGain, std::memory_order_release);

    auto outputDelay = (float)obs_data_get_double(s, OUTPUT_DELAY_ID);
    adio->outputDelay.store(outputDelay, std::memory_order_release);
    adio->deviceIo2.setOutputDelay(outputDelay);

    auto outputGain = (float)obs_data_get_double(s, OG_ID);
    adio->outputGainDb.store(outputGain, std::memory_order_release);
}

static void* deviceio2_create(obs_data_t* settings, obs_source_t* filter)
{
    struct adio2_data* adio = new adio2_data();
    adio->context = filter;

    auto numChannels = (int)audio_output_get_channels(obs_get_audio());
    auto sampleRate = audio_output_get_sample_rate(obs_get_audio());

    adio->channels = numChannels;
    adio->sampleRate = sampleRate;

    deviceio2_update(adio, settings);

    // Load state from settings if present (OBS load callback may not be called for all source types)
    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    if (chunkData && strlen(chunkData) > 0)
    {
        std::string stateStr = chunkData;
        adio->deviceIo2.setState(stateStr);
        adio->hasLoadedState = true;
    }

    return adio;
}

static void deviceio2_defaults(obs_data_t* s)
{
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

    adio2_data* adio = (adio2_data*)data;
    adio->deviceIo2.setVisible(true);

    return true;
}

static bool close_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    obs_property_set_visible(obs_properties_get(props, OPEN_DEVICE_SETTINGS), true);
    obs_property_set_visible(obs_properties_get(props, CLOSE_DEVICE_SETTINGS), false);

    adio2_data* adio = (adio2_data*)data;
    adio->deviceIo2.setVisible(false);

    return true;
}

static obs_properties_t* deviceio2_properties(void* data)
{
    obs_properties_t* props = obs_properties_create();

    obs_properties_add_button2(props, OPEN_DEVICE_SETTINGS, OPEN_DEVICE_TEXT, open_editor_button_clicked, data);
    obs_properties_add_button2(props, CLOSE_DEVICE_SETTINGS, CLOSE_DEVICE_TEXT, close_editor_button_clicked, data);

    bool open_settings_vis = true;
    bool close_settings_vis = false;

    obs_property_set_visible(obs_properties_get(props, OPEN_DEVICE_SETTINGS), open_settings_vis);
    obs_property_set_visible(obs_properties_get(props, CLOSE_DEVICE_SETTINGS), close_settings_vis);

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

static struct obs_audio_data* deviceio2_filter(void* data, struct obs_audio_data* audio)
{
    struct adio2_data* adio = (struct adio2_data*)data;
    auto channels = adio->channels;
    auto frames = audio->frames;
    float** adata = (float**)audio->data;

    atk::DeviceIo2::WrapperControlState controlState;
    controlState.followSourceVolume = adio->followSourceVolume.load(std::memory_order_acquire);
    controlState.followScene = adio->followScene.load(std::memory_order_acquire);
    controlState.inputGainDb = adio->inputGainDb.load(std::memory_order_acquire);
    controlState.outputGainDb = adio->outputGainDb.load(std::memory_order_acquire);
    controlState.sourceVolume = adio->sourceVolume.load(std::memory_order_acquire);
    controlState.sourceMuted = adio->sourceMuted.load(std::memory_order_acquire);
    controlState.fadeTimeSeconds = adio->fadeTimeSeconds.load(std::memory_order_acquire);
    controlState.hasDesiredBypass = adio->hasDesiredBypass;
    controlState.desiredBypass = adio->desiredBypass;
    adio->deviceIo2.setWrapperControlState(controlState);

    adio->deviceIo2.process(adata, channels, frames, adio->sampleRate);

    return audio;
}

static void save(void* data, obs_data_t* settings)
{
    auto* adio = (struct adio2_data*)data;
    std::string s;
    adio->deviceIo2.getState(s);

    obs_data_set_string(settings, FILTER_ID, s.c_str());
}

static void tick(void* data, float seconds)
{
    struct adio2_data* adio = (struct adio2_data*)data;

    // Cache transition duration from frontend API (called on main thread)
    if (adio->followScene.load(std::memory_order_acquire))
    {
        adio->fadeTimeSeconds.store(
            atk::deviceIo2ObsControl::getCurrentTransitionFadeSeconds(),
            std::memory_order_release
        );
    }

    // Compute bypass state on main thread for audio thread to read
    obs_source_t* parent = obs_filter_get_parent(adio->context);

    bool bypass = false;
    if (adio->followScene.load(std::memory_order_acquire) && parent)
    {
        bool desiredBypass = true;
        auto* parentUuid = obs_source_get_uuid(parent);
        std::string parentUuidString = parentUuid ? parentUuid : "";

        if (atk::deviceIo2ObsControl::resolveDesiredBypass(parentUuidString, desiredBypass))
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
    adio->desiredBypass = bypass;

    if (adio->followSourceVolume.load(std::memory_order_acquire) && parent)
    {
        auto* parentUuid = obs_source_get_uuid(parent);
        std::string parentUuidString = parentUuid ? parentUuid : "";

        atk::deviceIo2ObsControl::SourceLevelState sourceLevelState;
        if (atk::deviceIo2ObsControl::readSourceLevelState(parentUuidString, sourceLevelState))
        {
            adio->sourceMuted.store(sourceLevelState.sourceMuted, std::memory_order_release);
            adio->sourceVolume.store(sourceLevelState.sourceVolume, std::memory_order_release);
        }
        else
        {
            adio->sourceMuted.store(false, std::memory_order_release);
            adio->sourceVolume.store(1.0f, std::memory_order_release);
        }
    }
    else
    {
        adio->sourceMuted.store(false, std::memory_order_release);
        adio->sourceVolume.store(1.0f, std::memory_order_release);
    }

    UNUSED_PARAMETER(seconds);
}

struct obs_source_info device_io2_filter = {
    .id = FILTER_ID,
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = deviceio2_name,
    .create = deviceio2_create,
    .destroy = deviceio2_destroy,
    .get_defaults = deviceio2_defaults,
    .get_properties = deviceio2_properties,
    .update = deviceio2_update,
    .video_tick = tick,
    .filter_audio = deviceio2_filter,
    .save = save,
    .load = load,
};
