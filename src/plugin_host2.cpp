#include <algorithm>
#include <atkaudio/PluginHost2/API/PluginHost2.h>
#include <inttypes.h>
#include <math.h>
#include <media-io/audio-math.h>
#include <mutex>
#include <obs-module.h>
#include <stdint.h>
#include <util/platform.h>
#include <vector>

#define FILTER_NAME "atkAudio PluginHost2"
#define FILTER_ID "atkaudio_plugin_host2"

#define OPEN_PLUGIN_SETTINGS "open_filter_graph"
#define OPEN_PLUGIN_TEXT "Open Filter Graph"
#define CLOSE_PLUGIN_SETTINGS "close_filter_graph"
#define CLOSE_PLUGIN_TEXT "Close Filter Graph"

struct pluginhost2_data
{
    obs_source_t* context;
    obs_source_t* parent;

    atk::PluginHost2 pluginHost2;

    size_t num_channels;
    size_t sample_rate;

    bool hasLoadedState = false;
};

static const char* pluginhost2_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text(FILTER_NAME);
}

static void save(void* data, obs_data_t* settings)
{
    auto* ph = (struct pluginhost2_data*)data;
    std::string s;
    ph->pluginHost2.getState(s);

    obs_data_set_string(settings, FILTER_ID, s.c_str());
}

static void load(void* data, obs_data_t* settings)
{
    auto* ph = (struct pluginhost2_data*)data;
    if (ph->hasLoadedState)
        return;
    ph->hasLoadedState = true;

    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    std::string stateStr = chunkData ? chunkData : "";
    ph->pluginHost2.setState(stateStr);
}

static void pluginhost2_update(void* data, obs_data_t* s)
{
    struct pluginhost2_data* ph = (struct pluginhost2_data*)data;

    const uint32_t sample_rate = audio_output_get_sample_rate(obs_get_audio());
    const size_t num_channels = audio_output_get_channels(obs_get_audio());

    ph->num_channels = num_channels;
    ph->sample_rate = sample_rate;
}

static void* pluginhost2_create(obs_data_t* settings, obs_source_t* filter)
{
    struct pluginhost2_data* ph = new pluginhost2_data();
    ph->context = filter;
    ph->parent = obs_filter_get_parent(filter);
    ph->pluginHost2.setParentSource(ph->parent);

    pluginhost2_update(ph, settings);

    // Load state from settings if present (OBS load callback may not be called for all source types)
    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    if (chunkData && strlen(chunkData) > 0)
    {
        std::string stateStr = chunkData;
        ph->pluginHost2.setState(stateStr);
        ph->hasLoadedState = true;
    }

    return ph;
}

static void pluginhost2_destroy(void* data)
{
    struct pluginhost2_data* ph = (struct pluginhost2_data*)data;

    delete ph;
}

static struct obs_audio_data* pluginhost2_filter_audio(void* data, struct obs_audio_data* audio)
{
    struct pluginhost2_data* ph = (struct pluginhost2_data*)data;

    int num_samples = audio->frames;
    if (num_samples == 0)
        return audio;

    float** samples = (float**)audio->data;

    ph->pluginHost2.process(samples, (int)ph->num_channels, num_samples, (double)ph->sample_rate);

    return audio;
}

static bool open_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    obs_property_set_visible(obs_properties_get(props, OPEN_PLUGIN_SETTINGS), false);
    obs_property_set_visible(obs_properties_get(props, CLOSE_PLUGIN_SETTINGS), true);

    pluginhost2_data* ph = (pluginhost2_data*)data;
    ph->pluginHost2.setVisible(true);

    return true;
}

static bool close_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    obs_property_set_visible(obs_properties_get(props, OPEN_PLUGIN_SETTINGS), true);
    obs_property_set_visible(obs_properties_get(props, CLOSE_PLUGIN_SETTINGS), false);

    pluginhost2_data* ph = (pluginhost2_data*)data;
    ph->pluginHost2.setVisible(false);

    return true;
}

static obs_properties_t* pluginhost2_properties(void* data)
{
    struct pluginhost2_data* ph = (struct pluginhost2_data*)data;
    obs_properties_t* props = obs_properties_create();
    obs_source_t* parent = NULL;

    if (ph)
        parent = obs_filter_get_parent(ph->context);

    obs_properties_add_button(props, OPEN_PLUGIN_SETTINGS, OPEN_PLUGIN_TEXT, open_editor_button_clicked);
    obs_properties_add_button(props, CLOSE_PLUGIN_SETTINGS, CLOSE_PLUGIN_TEXT, close_editor_button_clicked);

    bool open_settings_vis = true;
    bool close_settings_vis = false;

    obs_property_set_visible(obs_properties_get(props, OPEN_PLUGIN_SETTINGS), open_settings_vis);
    obs_property_set_visible(obs_properties_get(props, CLOSE_PLUGIN_SETTINGS), close_settings_vis);

    return props;
}

static void pluginhost2_filter_add(void* data, obs_source_t* source)
{
    struct pluginhost2_data* ph = (struct pluginhost2_data*)data;
    ph->parent = source;
    ph->pluginHost2.setParentSource(source);
}

struct obs_source_info pluginhost2_filter = {
    .id = FILTER_ID,
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = pluginhost2_name,
    .create = pluginhost2_create,
    .destroy = pluginhost2_destroy,
    .get_properties = pluginhost2_properties,
    .update = pluginhost2_update,
    .filter_audio = pluginhost2_filter_audio,
    .save = save,
    .load = load,
    .filter_add = pluginhost2_filter_add,
};