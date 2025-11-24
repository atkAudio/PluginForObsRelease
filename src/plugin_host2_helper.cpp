#include "config.h"

#include <atkaudio/PluginHost2/Core/ObsOutput.h>
#include <obs-module.h>

#define SOURCE_NAME "atkAudio Ph2 OBS Output Node"
#define SOURCE_ID "atkaudio_ph2helper"

struct ph2helper_data
{
    obs_source_t* source;
};

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
    delete ph2h;
}

static void* create(obs_data_t* settings, obs_source_t* source)
{
    UNUSED_PARAMETER(settings);
    UNUSED_PARAMETER(source);
    return new ph2helper_data();
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