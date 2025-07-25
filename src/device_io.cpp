#include <atkaudio/DeviceIo/DeviceIo.h>
#include <atomic>
#include <obs-module.h>

#define FILTER_NAME "atkAudio Device I/O"
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

struct adio_data
{
    obs_source_t* context = nullptr;
    obs_data_t* settings = nullptr;

    int channels;
    double sampleRate = 0.0;

    std::atomic_bool mixInput = false;
    std::atomic_bool followSourceVolume = false;
    std::atomic<float> inputGain = 1.0f;
    std::atomic<float> outputGain = 1.0f;

    atk::DeviceIo deviceIo;
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
    std::string s;
    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    s = chunkData;
    adio->deviceIo.setState(s);
}

static void devio_update(void* data, obs_data_t* s)
{
    struct adio_data* adio = (struct adio_data*)data;
    adio->settings = s;
    adio->channels = (int)audio_output_get_channels(obs_get_audio());

    adio->mixInput.store(obs_data_get_bool(s, S_MIX_INPUT), std::memory_order_release);
    adio->followSourceVolume.store(obs_data_get_bool(s, FOLLOW_ID), std::memory_order_release);

    auto inputGain = (float)obs_data_get_double(s, IG_ID);
    inputGain = obs_db_to_mul(inputGain);
    adio->inputGain.store(inputGain, std::memory_order_release);

    // auto outputGain = (float)obs_data_get_double(s, OG_ID);
    // outputGain = obs_db_to_mul(outputGain);
    // adio->outputGain.store(outputGain, std::memory_order_release);

    // load(data, s);
}

static void* devio_create(obs_data_t* settings, obs_source_t* filter)
{
    struct adio_data* adio = new adio_data();
    adio->context = filter;

    auto numChannels = (int)audio_output_get_channels(obs_get_audio());
    auto sampleRate = audio_output_get_sample_rate(obs_get_audio());

    adio->channels = numChannels;
    adio->sampleRate = sampleRate;

    devio_update(adio, settings);

    return adio;
}

static void devio_defaults(obs_data_t* s)
{
    obs_data_set_default_bool(s, S_MIX_INPUT, false);
    obs_data_set_default_bool(s, FOLLOW_ID, false);
    obs_data_set_default_double(s, IG_ID, 0.0);
    obs_data_set_default_double(s, OG_ID, 0.0);
}

static bool open_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    obs_property_set_visible(obs_properties_get(props, OPEN_DEVICE_SETTINGS), false);
    obs_property_set_visible(obs_properties_get(props, CLOSE_DEVICE_SETTINGS), true);

    adio_data* adio = (adio_data*)data;
    adio->deviceIo.setVisible(true);

    return true;
}

static bool close_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    obs_property_set_visible(obs_properties_get(props, OPEN_DEVICE_SETTINGS), true);
    obs_property_set_visible(obs_properties_get(props, CLOSE_DEVICE_SETTINGS), false);

    adio_data* adio = (adio_data*)data;
    adio->deviceIo.setVisible(false);

    return true;
}

static obs_properties_t* devio_properties(void* data)
{
    obs_properties_t* props = obs_properties_create();

    obs_properties_add_button(props, OPEN_DEVICE_SETTINGS, OPEN_DEVICE_TEXT, open_editor_button_clicked);
    obs_properties_add_button(props, CLOSE_DEVICE_SETTINGS, CLOSE_DEVICE_TEXT, close_editor_button_clicked);

    bool open_settings_vis = true;
    bool close_settings_vis = false;

    obs_property_set_visible(obs_properties_get(props, OPEN_DEVICE_SETTINGS), open_settings_vis);
    obs_property_set_visible(obs_properties_get(props, CLOSE_DEVICE_SETTINGS), close_settings_vis);

    obs_properties_add_bool(props, S_MIX_INPUT, obs_module_text(MIX_INPUT_TEXT));

    std::string propText = FOLLOW_ID;
    std::string textLabel = FOLLOW_NAME;

    obs_properties_add_bool(props, propText.c_str(), textLabel.c_str());

    propText = IG_ID;
    textLabel = IG_NAME;
    obs_property_t* p = obs_properties_add_float_slider(props, propText.c_str(), textLabel.c_str(), -30.0, 30.0, 0.1);
    obs_property_float_set_suffix(p, " dB");

    propText = OG_ID;
    textLabel = OG_NAME;
    p = obs_properties_add_float_slider(props, propText.c_str(), textLabel.c_str(), -30.0, 30.0, 0.1);
    obs_property_float_set_suffix(p, " dB");

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
    for (int i = 0; i < channels; i++)
        for (size_t j = 0; j < frames; j++)
            adata[i][j] *= outputGain;

    adio->deviceIo.setMixInput(adio->mixInput.load(std::memory_order_acquire));
    adio->deviceIo.process(adata, channels, frames, adio->sampleRate);

    auto inputGain = adio->inputGain.load(std::memory_order_acquire);
    inputGain *= 1 / outputGain; // "remove" output gain from input gain
    for (int i = 0; i < channels; i++)
        for (size_t j = 0; j < frames; j++)
            adata[i][j] *= inputGain;

    return audio;
}

static void save(void* data, obs_data_t* settings)
{
    auto* adio = (struct adio_data*)data;
    std::string s;
    adio->deviceIo.getState(s);

    obs_data_set_string(settings, FILTER_ID, s.c_str());
}

static void tick(void* data, float seconds)
{
    struct adio_data* adio = (struct adio_data*)data;
    auto* settings = adio->settings;

    auto outputGain = adio->outputGain.load(std::memory_order_acquire);
    if (settings)
        outputGain = (float)obs_data_get_double(settings, OG_ID);
    outputGain = obs_db_to_mul(outputGain);

    if (adio->followSourceVolume.load(std::memory_order_acquire))
    {
        obs_source_t* parent = obs_filter_get_parent(adio->context);
        if (parent)
        {
            auto fader = obs_source_get_volume(parent); // already in linear scale

            auto isMuted = obs_source_muted(parent);
            if (isMuted)
                fader = 0.0f;

            outputGain *= fader;
        }
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
