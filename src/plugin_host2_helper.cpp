#include "config.h"

#include <atkaudio/PluginHost2/Core/ObsOutput.h>
#include <obs-module.h>

#define SOURCE_NAME "atkAudio PluginHost2 Helper"
#define SOURCE_ID "atkaudio_ph2helper"

struct ph2helper_data
{
    obs_source_t* source;
};

void audio_output_callback(void* param, size_t mix_idx, audio_data* data)
{
    UNUSED_PARAMETER(param);
    UNUSED_PARAMETER(mix_idx);
    UNUSED_PARAMETER(data);
}

static void destroy(void* data)
{
    auto* ph2h = static_cast<ph2helper_data*>(data);
    if (!ph2h)
        return;
    if (ph2h->source)
    {
        obs_source_release(ph2h->source);
        ph2h->source = nullptr;
    }
    for (int i = 0; i < MAX_AUDIO_MIXES; ++i)
        obs_remove_raw_audio_callback(i, audio_output_callback, ph2h);
    delete ph2h;
}

static void update(void* data, obs_data_t* s)
{
}

static void* create(obs_data_t* settings, obs_source_t* source)
{
    auto* ph2h = new ph2helper_data();
    for (int i = 0; i < MAX_AUDIO_MIXES; ++i)
        obs_add_raw_audio_callback(i, nullptr, audio_output_callback, ph2h);
    return ph2h;
}

static const char* getname(void* unused)
{
    UNUSED_PARAMETER(unused);
    return SOURCE_NAME;
}

static obs_properties_t* properties(void* data)
{
    obs_properties_t* props = obs_properties_create();

    return props;
}

struct obs_source_info ph2helper_source_info = {
    .id = SOURCE_ID,
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = getname,
    .create = create,
    .destroy = destroy,
};