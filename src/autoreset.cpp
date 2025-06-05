#include <algorithm>
#include <math.h>
#include <media-io/audio-math.h>
#include <mutex>
#include <obs-module.h>
#include <shared_mutex>
#include <vector>

#define FILTER_NAME "atkAuto-Reset Monitoring Devices"
#define FILTER_ID "atkauto_reset_monitoring_devices"
#define INTERVAL_ID "interval"
#define INTERVAL_NAME "Interval (minutes)"

struct autoreset_data
{
    obs_source_t* context;

    static inline std::vector<autoreset_data*> instances;
    static inline std::shared_mutex sharedMutex;

    float tickSecondsState = 0.0f;
    float interval = 900.0f;

    autoreset_data()
    {
        std::unique_lock lock(sharedMutex);
        instances.push_back(this);
    }

    ~autoreset_data()
    {
        std::unique_lock lock(sharedMutex);
        instances.erase(std::remove(instances.begin(), instances.end(), this), instances.end());
    }
};

static const char* autoreset_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text(FILTER_NAME);
}

static void autoreset_destroy(void* data)
{
    struct autoreset_data* ard = (struct autoreset_data*)data;
    delete ard;
}

static void autoreset_update(void* data, obs_data_t* s)
{
    struct autoreset_data* ard = (struct autoreset_data*)data;

    ard->interval = (float)obs_data_get_double(s, INTERVAL_ID);
}

static void* autoreset_create(obs_data_t* settings, obs_source_t* filter)
{
    auto* ard = new autoreset_data();
    autoreset_update(ard, settings);
    return ard;
}

static struct obs_audio_data* autoreset_filter_audio(void* data, struct obs_audio_data* audio)
{
    struct autoreset_data* ard = (struct autoreset_data*)data;

    return audio;
}

static void autoreset_defaults(obs_data_t* s)
{
    obs_data_set_default_double(s, INTERVAL_ID, 15.0);
    UNUSED_PARAMETER(s);
}

static obs_properties_t* autoreset_properties(void* data)
{
    struct autoreset_data* ard = (struct autoreset_data*)data;
    obs_properties_t* props = obs_properties_create();

    obs_property_t* p =
        obs_properties_add_float_slider(props, INTERVAL_ID, obs_module_text(INTERVAL_NAME), 1.0, 1440.0, 1.0);

    obs_property_set_long_description(
        p,
        obs_module_text("Set the interval in minutes for auto-resetting the monitoring devices. Can help with "
                        "audio monitoring drifting out of sync. Can cause a glitch or brief dropout in monitoring.")
    );

    UNUSED_PARAMETER(data);
    return props;
}

static void autoreset_tick(void* data, float seconds)
{
    struct autoreset_data* ard = (struct autoreset_data*)data;
    ard->tickSecondsState += seconds;
    if (ard->tickSecondsState > ard->interval * 60.0f)
    {
        ard->tickSecondsState = 0.0f;
        std::shared_lock lock(autoreset_data::sharedMutex);
        if (ard == autoreset_data::instances[0])
            obs_reset_audio_monitoring();
    }
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(seconds);
}

struct obs_source_info autoreset_filter = {
    .id = FILTER_ID,
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = autoreset_name,
    .create = autoreset_create,
    .destroy = autoreset_destroy,
    .get_defaults = autoreset_defaults,
    .get_properties = autoreset_properties,
    .update = autoreset_update,
    .video_tick = autoreset_tick,
    .filter_audio = autoreset_filter_audio,
};