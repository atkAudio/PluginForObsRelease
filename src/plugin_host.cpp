#include <algorithm>
#include <atkaudio/FifoBuffer.h>
#include <atkaudio/PluginHost/PluginHost.h>
#include <inttypes.h>
#include <math.h>
#include <media-io/audio-math.h>
#include <mutex>
#include <obs-module.h>
#include <stdint.h>
#include <util/platform.h>
#include <vector>

#define FILTER_NAME "atkAudio Plugin Host"
#define FILTER_ID "atkaudio_plugin_host"

#define OPEN_PLUGIN_SETTINGS "open_plugin_settings"
#define OPEN_PLUGIN_TEXT "Open Plugin Settings"
#define CLOSE_PLUGIN_SETTINGS "close_plugin_settings"
#define CLOSE_PLUGIN_TEXT "Close Plugin Settings"

#define S_SIDECHAIN_SOURCE "sidechain_source"

#define MT_ obs_module_text
#define TEXT_SIDECHAIN_SOURCE MT_("Sidechain")

struct pluginhost_data
{
    obs_source_t* context;

    atk::PluginHost pluginHost;

    std::vector<std::vector<float>> sidechainTempBuffer;
    std::vector<float*> pointersToProcess;
    atk::FifoBuffer sidechain_fifo;

    size_t envelope_buf_len;

    size_t num_channels;
    size_t sample_rate;

    std::mutex sidechain_update_mutex;
    uint64_t sidechain_check_time;
    obs_weak_source_t* weak_sidechain;
    char* sidechain_name;

    std::mutex sidechain_mutex;
    size_t max_sidechain_frames;
};

/* -------------------------------------------------------- */

static inline obs_source_t* get_sidechain(struct pluginhost_data* ph)
{
    if (ph->weak_sidechain)
        return obs_weak_source_get_source(ph->weak_sidechain);
    return NULL;
}

static const char* pluginhost_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text(FILTER_NAME);
}

static void sidechain_capture(void* param, obs_source_t* source, const struct audio_data* audio_data, bool muted)
{
    struct pluginhost_data* ph = (struct pluginhost_data*)param;

    UNUSED_PARAMETER(source);

    if (ph->max_sidechain_frames < audio_data->frames)
        ph->max_sidechain_frames = audio_data->frames;

    size_t expected_size = ph->max_sidechain_frames * sizeof(float);

    if (muted)
        return;

    auto samples = (float**)audio_data->data;

    auto numChannels = ph->sidechain_fifo.getNumChannels();
    auto numSamples = ph->sidechain_fifo.getTotalSize() - 1;

    if (numChannels != ph->num_channels || numSamples < ph->max_sidechain_frames)
        ph->sidechain_fifo.setSize((int)ph->num_channels, (int)ph->max_sidechain_frames);

    for (int i = 0; i < ph->num_channels; i++)
    {
        auto* sourcePtr = samples[i];
        ph->sidechain_fifo.write(sourcePtr, i, audio_data->frames, i == ph->num_channels - 1);
    }
}

static void save(void* data, obs_data_t* settings)
{
    auto* ph = (struct pluginhost_data*)data;
    std::string s;
    ph->pluginHost.getState(s);

    obs_data_set_string(settings, FILTER_ID, s.c_str());
}

static void load(void* data, obs_data_t* settings)
{
    auto* ph = (struct pluginhost_data*)data;
    std::string s;
    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    s = chunkData;
    ph->pluginHost.setState(s);
}

static void pluginhost_update(void* data, obs_data_t* s)
{
    struct pluginhost_data* ph = (struct pluginhost_data*)data;

    const uint32_t sample_rate = audio_output_get_sample_rate(obs_get_audio());
    const size_t num_channels = audio_output_get_channels(obs_get_audio());
    const char* sidechain_name = obs_data_get_string(s, S_SIDECHAIN_SOURCE);

    ph->num_channels = num_channels;
    ph->sample_rate = sample_rate;

    bool valid_sidechain = *sidechain_name && strcmp(sidechain_name, "none") != 0;
    obs_weak_source_t* old_weak_sidechain = NULL;

    ph->sidechain_update_mutex.lock();

    if (!valid_sidechain)
    {
        if (ph->weak_sidechain)
        {
            old_weak_sidechain = ph->weak_sidechain;
            ph->weak_sidechain = NULL;
        }

        bfree(ph->sidechain_name);
        ph->sidechain_name = NULL;
    }
    else if (!ph->sidechain_name || strcmp(ph->sidechain_name, sidechain_name) != 0)
    {
        if (ph->weak_sidechain)
        {
            old_weak_sidechain = ph->weak_sidechain;
            ph->weak_sidechain = NULL;
        }

        bfree(ph->sidechain_name);
        ph->sidechain_name = bstrdup(sidechain_name);
        ph->sidechain_check_time = os_gettime_ns() - 3000000000;
    }

    ph->sidechain_update_mutex.unlock();

    if (old_weak_sidechain)
    {
        obs_source_t* old_sidechain = obs_weak_source_get_source(old_weak_sidechain);

        if (old_sidechain)
        {
            obs_source_remove_audio_capture_callback(old_sidechain, sidechain_capture, ph);
            obs_source_release(old_sidechain);
        }

        obs_weak_source_release(old_weak_sidechain);
    }

    // load state
    load(data, s);
}

static void* pluginhost_create(obs_data_t* settings, obs_source_t* filter)
{
    struct pluginhost_data* ph = new pluginhost_data();
    ph->context = filter;

    pluginhost_update(ph, settings);

    ph->pointersToProcess.resize(ph->num_channels * 2, nullptr);
    ph->sidechainTempBuffer.resize(ph->num_channels, std::vector<float>(AUDIO_OUTPUT_FRAMES, 0.0f));
    ph->sidechain_fifo.setSize((int)ph->num_channels, AUDIO_OUTPUT_FRAMES);
    return ph;
}

static void pluginhost_destroy(void* data)
{
    struct pluginhost_data* ph = (struct pluginhost_data*)data;

    if (ph->weak_sidechain)
    {
        obs_source_t* sidechain = get_sidechain(ph);
        if (sidechain)
        {
            obs_source_remove_audio_capture_callback(sidechain, sidechain_capture, ph);
            obs_source_release(sidechain);
        }

        obs_weak_source_release(ph->weak_sidechain);
    }

    bfree(ph->sidechain_name);
    delete ph;
}

static void pluginhost_tick(void* data, float seconds)
{
    struct pluginhost_data* ph = (struct pluginhost_data*)data;
    char* new_name = NULL;

    ph->sidechain_update_mutex.lock();

    if (ph->sidechain_name && !ph->weak_sidechain)
    {
        uint64_t t = os_gettime_ns();

        if (t - ph->sidechain_check_time > 3000000000)
        {
            new_name = bstrdup(ph->sidechain_name);
            ph->sidechain_check_time = t;
        }
    }

    ph->sidechain_update_mutex.unlock();

    if (new_name)
    {
        obs_source_t* sidechain = *new_name ? obs_get_source_by_name(new_name) : NULL;
        obs_weak_source_t* weak_sidechain = sidechain ? obs_source_get_weak_source(sidechain) : NULL;

        ph->sidechain_update_mutex.lock();

        if (ph->sidechain_name && strcmp(ph->sidechain_name, new_name) == 0)
        {
            ph->weak_sidechain = weak_sidechain;
            weak_sidechain = NULL;
        }

        ph->sidechain_update_mutex.unlock();

        if (sidechain)
        {
            obs_source_add_audio_capture_callback(sidechain, sidechain_capture, ph);

            obs_weak_source_release(weak_sidechain);
            obs_source_release(sidechain);
        }

        bfree(new_name);
    }

    UNUSED_PARAMETER(seconds);
}

static struct obs_audio_data* pluginhost_filter_audio(void* data, struct obs_audio_data* audio)
{
    struct pluginhost_data* ph = (struct pluginhost_data*)data;

    int num_samples = audio->frames;
    if (num_samples == 0)
        return audio;

    float** samples = (float**)audio->data;

    // num_channels should be the number of main bus channels
    // buffer should hold 2x number of channels (main bus + sidechain)
    // if no sidechain, then second half of buffer should be zeroed

    ph->sidechainTempBuffer.resize(ph->num_channels);
    for (auto& i : ph->sidechainTempBuffer)
    {
        std::fill(i.begin(), i.end(), 0.0f);
        if (i.size() < num_samples)
            i.resize(num_samples, 0.0f);
    }

    auto numSidechainSamplesReady = ph->sidechain_fifo.getNumReady();
    if (numSidechainSamplesReady >= num_samples)
        for (int i = 0; i < ph->num_channels; i++)
            ph->sidechain_fifo.read(ph->sidechainTempBuffer[i].data(), i, num_samples, i == ph->num_channels - 1);

    numSidechainSamplesReady = ph->sidechain_fifo.getNumReady();
    if (numSidechainSamplesReady >= num_samples)
        ph->sidechain_fifo.advanceRead(numSidechainSamplesReady);

    ph->pointersToProcess.resize(ph->num_channels * 2);

    for (size_t i = 0; i < ph->num_channels; i++)
    {
        ph->pointersToProcess[i] = samples[i];
        ph->pointersToProcess[i + ph->num_channels] = ph->sidechainTempBuffer[i].data();
    }

    ph->pluginHost.process(ph->pointersToProcess.data(), (int)ph->num_channels, num_samples, (double)ph->sample_rate);

    return audio;
}

static void pluginhost_defaults(obs_data_t* s)
{
    obs_data_set_default_string(s, S_SIDECHAIN_SOURCE, "none");
}

struct sidechain_prop_info
{
    obs_property_t* sources;
    obs_source_t* parent;
};

static bool add_sources(void* data, obs_source_t* source)
{
    struct sidechain_prop_info* info = (struct sidechain_prop_info*)data;
    uint32_t caps = obs_source_get_output_flags(source);

    if (source == info->parent)
        return true;
    if ((caps & OBS_SOURCE_AUDIO) == 0)
        return true;

    const char* name = obs_source_get_name(source);
    obs_property_list_add_string(info->sources, name, name);
    return true;
}

static bool open_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    obs_property_set_visible(obs_properties_get(props, OPEN_PLUGIN_SETTINGS), false);
    obs_property_set_visible(obs_properties_get(props, CLOSE_PLUGIN_SETTINGS), true);

    pluginhost_data* ph = (pluginhost_data*)data;
    ph->pluginHost.setVisible(true);

    return true;
}

static bool close_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    obs_property_set_visible(obs_properties_get(props, OPEN_PLUGIN_SETTINGS), true);
    obs_property_set_visible(obs_properties_get(props, CLOSE_PLUGIN_SETTINGS), false);

    pluginhost_data* ph = (pluginhost_data*)data;
    ph->pluginHost.setVisible(false);

    return true;
}

static obs_properties_t* pluginhost_properties(void* data)
{
    struct pluginhost_data* ph = (struct pluginhost_data*)data;
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

    obs_property_t* sources = obs_properties_add_list(
        props,
        S_SIDECHAIN_SOURCE,
        TEXT_SIDECHAIN_SOURCE,
        OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING
    );

    obs_property_list_add_string(sources, obs_module_text("None"), "none");

    struct sidechain_prop_info info = {sources, parent};
    obs_enum_sources(add_sources, &info);

    return props;
}

struct obs_source_info pluginhost_filter = {
    .id = FILTER_ID,
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = pluginhost_name,
    .create = pluginhost_create,
    .destroy = pluginhost_destroy,
    .get_defaults = pluginhost_defaults,
    .get_properties = pluginhost_properties,
    .update = pluginhost_update,
    .video_tick = pluginhost_tick,
    .filter_audio = pluginhost_filter_audio,
    .save = save,
    // .load = load,
};