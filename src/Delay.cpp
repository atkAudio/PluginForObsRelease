#include <atkaudio/Delay.h>
#include <obs-module.h>

#define FILTER_NAME "atkAudio Delay"
#define FILTER_ID "atkaudio_delay"
#define MAX_DELAY_MS 10000.0

#define S_GAIN_DB "ms"

#define MT_ obs_module_text
#define TEXT_GAIN_DB MT_("Delay")

struct delay_data
{
    obs_source_t* context;
    size_t channels;
    double sample_rate;

    atk::Delay delayProcessor;

    float delay;
};

static const char* delay_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text(FILTER_NAME);
}

static void delay_destroy(void* data)
{
    struct delay_data* df = (struct delay_data*)data;
    delete df;
}

static void delay_update(void* data, obs_data_t* s)
{
    struct delay_data* df = (struct delay_data*)data;
    double val = obs_data_get_double(s, S_GAIN_DB);
    df->delay = (float)val;
}

static void* delay_create(obs_data_t* settings, obs_source_t* filter)
{
    struct delay_data* df = new delay_data();
    df->context = filter;

    df->channels = audio_output_get_channels(obs_get_audio());
    df->sample_rate = audio_output_get_sample_rate(obs_get_audio());

    delay_update(df, settings);

    return df;
}

static struct obs_audio_data* delay_filter_audio(void* data, struct obs_audio_data* audio)
{
    struct delay_data* df = (struct delay_data*)data;
    const size_t channels = df->channels;
    float** adata = (float**)audio->data;

    df->delayProcessor.setDelay(df->delay);
    df->delayProcessor.process(adata, (int)channels, audio->frames, df->sample_rate);

    return audio;
}

static void delay_defaults(obs_data_t* s)
{
    obs_data_set_default_double(s, S_GAIN_DB, 0.0f);
}

static obs_properties_t* delay_properties(void* data)
{
    obs_properties_t* ppts = obs_properties_create();

    obs_property_t* p = obs_properties_add_float_slider(ppts, S_GAIN_DB, TEXT_GAIN_DB, 0.0, MAX_DELAY_MS, 0.1);
    obs_property_float_set_suffix(p, " ms");

    UNUSED_PARAMETER(data);
    return ppts;
}

struct obs_source_info delay_filter = {
    .id = FILTER_ID,
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = delay_name,
    .create = delay_create,
    .destroy = delay_destroy,
    .get_defaults = delay_defaults,
    .get_properties = delay_properties,
    .update = delay_update,
    .filter_audio = delay_filter_audio,
};