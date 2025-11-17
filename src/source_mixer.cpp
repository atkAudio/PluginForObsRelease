#include "config.h"

#include <algorithm>
#include <atkaudio/FifoBuffer.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <obs-module.h>
#include <obs.h>
#include <string>
#include <util/platform.h>
#include <vector>

#define SOURCE_NAME "atkAudio Source Mixer"
#define SOURCE_ID "atkaudio_source_mixer"

#define MT_ obs_module_text
#define S_SIDECHAIN_SOURCE "sidechain_source"
#define TEXT_SIDECHAIN_SOURCE MT_("Source")
#define S_GAIN_DB "dB"
#define TEXT_GAIN_DB MT_("Gain")
#define S_OUTPUT "output"
#define TEXT_OUTPUT MT_("Output")
#define S_POSTMUTE "post_mute"
#define TEXT_POSTMUTE MT_("Post-Mute")
#define TEXT_LAYOUT MT_("Output Channels")
#define TEXT_LAYOUT_DEFAULT MT_("Default")
#define TEXT_LAYOUT_MONO MT_("Mono")
#define TEXT_LAYOUT_STEREO MT_("Stereo")
#define S_LAYOUT "layout"

struct source_data
{
    obs_source_t* source;
    obs_weak_source_t* weak_sidechain;
    char* sidechain_name;
    uint64_t sidechain_check_time;
    // uint64_t last_callback_time = 0;

    std::atomic_int frames = 0;

    float gain;
    bool postMute;

    atk::FifoBuffer fifoBuffer;
    std::vector<float> tempBuffer;

    std::atomic_bool isActive = false;
    std::atomic_uint64_t last_callback_time = 0;
};

struct audiosourcemixer_data
{
    std::unique_ptr<std::vector<source_data>> sources;

    std::atomic_int frames = 0;

    obs_source_t* source;
    obs_properties_t* properties = nullptr;

    obs_source_audio audio_data;

    std::mutex sidechain_update_mutex;

    size_t envelope_buf_len;

    std::mutex captureCallbackMutex;

    std::atomic_bool doRawCallback = false;
    std::atomic_int speaker_layout = 0;

    std::vector<std::vector<float>> tempBuffer;
    std::vector<float> tempBuffer2;
};

static void audio_output_callback(void* param, size_t mix_idx, struct audio_data* data)
{
    struct audiosourcemixer_data* asmd = (struct audiosourcemixer_data*)param;
    auto frames = (int)data->frames;
    asmd->frames = frames;

    if (!asmd->doRawCallback.load())
        return;

    std::scoped_lock lock(asmd->captureCallbackMutex);

    auto numChannels = audio_output_get_channels(obs_get_audio());
    auto sampleRate = audio_output_get_sample_rate(obs_get_audio());

    asmd->tempBuffer.resize(numChannels);
    for (auto& buffer : asmd->tempBuffer)
        buffer.resize(frames);

    for (auto& buffer : asmd->tempBuffer)
        std::fill(buffer.begin(), buffer.end(), 0.0f);

    asmd->tempBuffer2.resize(frames);
    std::fill(asmd->tempBuffer2.begin(), asmd->tempBuffer2.end(), 0.0f);

    for (int i = 0; i < numChannels; i++)
        asmd->audio_data.data[i] = (const uint8_t*)asmd->tempBuffer[i].data();

    asmd->audio_data.speakers =
        (speaker_layout)(asmd->speaker_layout.load() == 0 ? numChannels : asmd->speaker_layout.load());
    asmd->audio_data.samples_per_sec = sampleRate;
    asmd->audio_data.format = AUDIO_FORMAT_FLOAT_PLANAR;

    asmd->audio_data.frames = frames;
    asmd->audio_data.timestamp = data->timestamp;

    for (auto& source : *asmd->sources)
    {
        asmd->sidechain_update_mutex.lock();
        obs_weak_source_t* weak_sidechain = source.weak_sidechain;
        asmd->sidechain_update_mutex.unlock();

        // has sidechain
        if (weak_sidechain)
        {
            auto numReady = source.fifoBuffer.getNumReady();
            if (numReady < frames)
                continue;

            int asmdFrames = asmd->frames.load();
            int sourceFrames = source.frames.load();
            auto maxFrames = (std::max)(asmdFrames, sourceFrames);
            maxFrames = maxFrames * 2;

            if (numReady > maxFrames)
            {
                source.fifoBuffer.reset();

                continue;
            }

            numChannels = (std::min)((int)numChannels, source.fifoBuffer.getNumChannels());
            for (int i = 0; i < numChannels; i++)
            {
                auto* targetPtr = asmd->tempBuffer[i].data();
                auto* tempPtr = asmd->tempBuffer2.data();
                std::fill(asmd->tempBuffer2.begin(), asmd->tempBuffer2.end(), 0.0f);
                source.fifoBuffer.read(tempPtr, i, frames, i == numChannels - 1);

                for (size_t j = 0; j < frames; j++)
                    targetPtr[j] += tempPtr[j];
            }
        }
    }

    asmd->audio_data.timestamp = os_gettime_ns();

    obs_source_output_audio(asmd->source, &asmd->audio_data);
}

/* ------------------------------------------------------------------------- */

static const char* asmd_getname(void* unused)
{
    UNUSED_PARAMETER(unused);
    return SOURCE_NAME;
}

static inline obs_source_t* get_sidechain(source_data& sourceData, struct audiosourcemixer_data* asmd)
{
    if (sourceData.weak_sidechain)
        return obs_weak_source_get_source(sourceData.weak_sidechain);
    return nullptr;
}

static void asmd_capture(void* param, obs_source_t* sourceIn, const struct audio_data* audio_data, bool muted)
{
    struct audiosourcemixer_data* asmd = (struct audiosourcemixer_data*)param;

    source_data* source = nullptr;
    for (auto& src : *asmd->sources)
    {
        if (src.source == sourceIn)
        {
            source = &src;
            break;
        }
    }

    if (!source)
        return;

    source->isActive.store(false, std::memory_order_release);

    if (muted && source->postMute)
        return;

    source->isActive.store(true, std::memory_order_release);

    std::scoped_lock lock(asmd->captureCallbackMutex);

    auto frames = (int)audio_data->frames;

    source->frames.store(frames, std::memory_order_release);

    source->last_callback_time.store(audio_data->timestamp, std::memory_order_release);

    auto numChannels = (int)audio_output_get_channels(obs_get_audio());

    auto fifoSize = ((asmd->frames * 2) / frames + 1) * frames;

    source->tempBuffer.resize(fifoSize, 0.0f);
    source->fifoBuffer.setSize(numChannels, fifoSize);

    for (int i = 0; i < numChannels; i++)
    {
        auto* sourcePtr = (float*)audio_data->data[i];
        for (size_t j = 0; j < frames; j++)
            sourcePtr[j] *= source->gain;

        source->fifoBuffer.write(sourcePtr, i, frames, i == numChannels - 1);
    }

    auto sourcesFrameSize = frames;
    auto doThisCallback = true;
    for (auto& source : *asmd->sources)
    {
        auto sourceFrameSize = source.frames.load();
        if (sourceFrameSize == 0)
            continue;

        asmd->sidechain_update_mutex.lock();
        auto sourceRef = obs_weak_source_get_source(source.weak_sidechain);
        if (!sourceRef)
        {
            asmd->sidechain_update_mutex.unlock();
            continue;
        }
        obs_source_release(sourceRef);
        asmd->sidechain_update_mutex.unlock();

        if (sourceFrameSize != sourcesFrameSize)
        {
            doThisCallback = false;
            break;
        }
    }

    if (!doThisCallback)
    {
        asmd->doRawCallback = true;
        return;
    }
    asmd->doRawCallback = false;

    auto sampleRate = audio_output_get_sample_rate(obs_get_audio());

    for (auto& source : *asmd->sources)
    {
        asmd->sidechain_update_mutex.lock();
        auto sourceRef = obs_weak_source_get_source(source.weak_sidechain);
        if (!sourceRef)
        {
            asmd->sidechain_update_mutex.unlock();
            continue;
        }
        obs_source_release(sourceRef);
        asmd->sidechain_update_mutex.unlock();

        auto numReady = source.fifoBuffer.getNumReady();

        if (numReady < 2 * frames && source.isActive.load(std::memory_order_acquire))
            return;
    }

    asmd->tempBuffer.resize(numChannels);
    for (auto& buffer : asmd->tempBuffer)
        buffer.resize(frames);

    for (auto& buffer : asmd->tempBuffer)
        std::fill(buffer.begin(), buffer.end(), 0.0f);

    for (int i = 0; i < numChannels; i++)
        asmd->audio_data.data[i] = (const uint8_t*)asmd->tempBuffer[i].data();

    // auto sampleRate = audio_output_get_sample_rate(obs_get_audio());

    asmd->audio_data.speakers =
        (speaker_layout)(asmd->speaker_layout.load() == 0 ? numChannels : asmd->speaker_layout.load());
    asmd->audio_data.samples_per_sec = sampleRate;
    asmd->audio_data.format = AUDIO_FORMAT_FLOAT_PLANAR;

    asmd->audio_data.frames = frames;
    asmd->audio_data.timestamp = os_gettime_ns();

    for (auto& source : *asmd->sources)
    {
        asmd->sidechain_update_mutex.lock();
        obs_weak_source_t* weak_sidechain = source.weak_sidechain;
        asmd->sidechain_update_mutex.unlock();

        if (weak_sidechain)
        {
            auto numReady = source.fifoBuffer.getNumReady();

            if (numReady < frames || numReady > 2 * frames)
            {
                source.fifoBuffer.reset();

                continue;
            }

            numChannels = (std::min)((int)numChannels, source.fifoBuffer.getNumChannels());

            for (int i = 0; i < numChannels; i++)
            {
                auto* targetPtr = (float*)asmd->audio_data.data[i];
                source.tempBuffer.clear();
                auto* tempPtr = source.tempBuffer.data();
                source.fifoBuffer.read(tempPtr, i, frames, i == numChannels - 1);

                for (size_t j = 0; j < frames; j++)
                    targetPtr[j] += tempPtr[j];
            }
        }
    }

    asmd->audio_data.timestamp = os_gettime_ns();

    obs_source_output_audio(asmd->source, &asmd->audio_data);
}

static void asmd_destroy(void* data)
{
    struct audiosourcemixer_data* asmd = (struct audiosourcemixer_data*)data;

    for (auto& source : *asmd->sources)
    {
        if (source.weak_sidechain)
        {
            obs_source_t* sidechain = get_sidechain(source, asmd);
            if (sidechain)
            {
                source.source = nullptr;
                obs_source_remove_audio_capture_callback(sidechain, asmd_capture, asmd);
                obs_source_release(sidechain);
            }

            obs_weak_source_release(source.weak_sidechain);
        }
    }

    obs_remove_raw_audio_callback(0, audio_output_callback, asmd);

    delete asmd;
}

struct sidechain_prop_info
{
    obs_property_t* sources_list;
    obs_source_t* parent;
    audiosourcemixer_data* asmd;
};

static bool add_sources(void* data, obs_source_t* source)
{
    struct sidechain_prop_info* info = (struct sidechain_prop_info*)data;
    uint32_t caps = obs_source_get_output_flags(source);

    if (source == info->parent)
        return true;
    if ((caps & OBS_SOURCE_AUDIO) == 0)
        return true;
    if (info->asmd->source == source)
        return true;
    if (!obs_source_audio_active(source))
        return true;

    const char* name = obs_source_get_name(source);
    obs_property_list_add_string(info->sources_list, name, name);
    obs_properties_add_color(info->asmd->properties, name, name);

    return true;
}

static obs_properties_t* properties(void* data)
{
    struct audiosourcemixer_data* asmd = (struct audiosourcemixer_data*)data;

    obs_properties_t* props = asmd->properties;
    if (!props)
        props = obs_properties_create();

    auto* layout = obs_properties_add_list(props, S_LAYOUT, TEXT_LAYOUT, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

    obs_property_list_add_int(layout, TEXT_LAYOUT_DEFAULT, 0);
    obs_property_list_add_int(layout, TEXT_LAYOUT_MONO, 1);
    obs_property_list_add_int(layout, TEXT_LAYOUT_STEREO, 2);

    obs_source_t* parent = nullptr;

    if (asmd)
        parent = obs_filter_get_parent(asmd->source);

    for (int i = 0; i < MAX_AUDIO_CHANNELS; i++)
    {
        std::string propText = S_SIDECHAIN_SOURCE;
        propText += std::to_string(i + 1);
        std::string textLabel = TEXT_SIDECHAIN_SOURCE;
        textLabel += " " + std::to_string(i + 1);
        obs_property_t* sources = obs_properties_add_list(
            props,
            propText.c_str(),
            textLabel.c_str(),
            OBS_COMBO_TYPE_LIST,
            OBS_COMBO_FORMAT_STRING
        );

        obs_property_list_add_string(sources, obs_module_text("None"), "none");

        propText = S_GAIN_DB;
        propText += std::to_string(i + 1);
        textLabel = TEXT_GAIN_DB;
        textLabel += " " + std::to_string(i + 1);
        obs_property_t* p =
            obs_properties_add_float_slider(props, propText.c_str(), textLabel.c_str(), -30.0, 30.0, 0.1);
        obs_property_float_set_suffix(p, " dB");

        propText = S_POSTMUTE;
        propText += std::to_string(i + 1);
        textLabel = TEXT_POSTMUTE;
        textLabel += " " + std::to_string(i + 1);
        obs_properties_add_bool(props, propText.c_str(), textLabel.c_str());

        struct sidechain_prop_info info = {sources, parent, asmd};
        obs_enum_sources(add_sources, &info);
    }

    return props;
}

static void update(void* data, obs_data_t* s)
{
    struct audiosourcemixer_data* asmd = (struct audiosourcemixer_data*)data;

    // const uint32_t sample_rate = audio_output_get_sample_rate(obs_get_audio());
    // size_t num_channels = audio_output_get_channels(obs_get_audio());

    auto layout = obs_data_get_int(s, S_LAYOUT);

    asmd->speaker_layout.store((int)layout);

    int i = 0;
    for (auto& source : *asmd->sources)
    {
        std::string sidechainText = S_SIDECHAIN_SOURCE;
        sidechainText += std::to_string(i + 1);

        std::string paramText = S_GAIN_DB;
        paramText += std::to_string(i + 1);
        auto gain = (float)obs_data_get_double(s, paramText.c_str());
        gain = obs_db_to_mul(gain);
        source.gain = gain;

        paramText = S_POSTMUTE;
        paramText += std::to_string(i + 1);
        source.postMute = obs_data_get_bool(s, paramText.c_str());

        const char* sidechain_name = obs_data_get_string(s, sidechainText.c_str());

        bool valid_sidechain = *sidechain_name && strcmp(sidechain_name, "none") != 0;
        obs_weak_source_t* old_weak_sidechain = nullptr;

        asmd->sidechain_update_mutex.lock();

        if (!valid_sidechain)
        {
            if (source.weak_sidechain)
            {
                old_weak_sidechain = source.weak_sidechain;
                source.weak_sidechain = nullptr;
            }

            bfree(source.sidechain_name);
            source.sidechain_name = nullptr;
        }
        else if (!source.sidechain_name || strcmp(source.sidechain_name, sidechain_name) != 0)
        {
            if (source.weak_sidechain)
            {
                old_weak_sidechain = source.weak_sidechain;
                source.weak_sidechain = nullptr;
            }

            bfree(source.sidechain_name);
            source.sidechain_name = bstrdup(sidechain_name);
            source.sidechain_check_time = os_gettime_ns() - 3000000000;
        }

        asmd->sidechain_update_mutex.unlock();

        if (old_weak_sidechain)
        {
            obs_source_t* old_sidechain = obs_weak_source_get_source(old_weak_sidechain);

            if (old_sidechain)
            {
                source.source = nullptr;
                obs_source_remove_audio_capture_callback(old_sidechain, asmd_capture, asmd);
                obs_source_release(old_sidechain);
            }

            obs_weak_source_release(old_weak_sidechain);
        }
        i++;
    }
}

static void* asmd_create(obs_data_t* settings, obs_source_t* source)
{
    auto* asmd = new audiosourcemixer_data();

    asmd->source = source;
    asmd->sources.reset(new std::vector<source_data>(MAX_AUDIO_CHANNELS));

    for (auto& source : *asmd->sources)
        source.fifoBuffer.setSize((int)audio_output_get_channels(obs_get_audio()), AUDIO_OUTPUT_FRAMES);

    asmd->audio_data.format = AUDIO_FORMAT_FLOAT_PLANAR;

    update(asmd, settings);

    obs_add_raw_audio_callback(0, nullptr, audio_output_callback, asmd);

    UNUSED_PARAMETER(settings);
    return asmd;
}

static void compressor_defaults(obs_data_t* s)
{
    obs_data_set_default_string(s, S_SIDECHAIN_SOURCE, "none");
}

static void asmd_tick(void* data, float seconds)
{
    struct audiosourcemixer_data* asmd = (struct audiosourcemixer_data*)data;

    uint64_t t = os_gettime_ns();

    auto sampleRate = audio_output_get_sample_rate(obs_get_audio());

    for (auto& source : *asmd->sources)
    {
        char* new_name = nullptr;
        asmd->sidechain_update_mutex.lock();

        if (t - source.last_callback_time.load(std::memory_order_acquire)
            > (2.0 * source.frames.load()) / sampleRate * 1000000000)
            source.isActive.store(false, std::memory_order_release);

        if (source.sidechain_name && !source.weak_sidechain)
        {
            if (t - source.sidechain_check_time > 3000000000)
            {
                new_name = bstrdup(source.sidechain_name);
                source.sidechain_check_time = t;
            }
        }

        asmd->sidechain_update_mutex.unlock();

        if (new_name)
        {
            obs_source_t* sidechain = *new_name ? obs_get_source_by_name(new_name) : nullptr;
            obs_weak_source_t* weak_sidechain = sidechain ? obs_source_get_weak_source(sidechain) : nullptr;

            for (auto& source : *asmd->sources)
            {
                if (source.weak_sidechain == weak_sidechain)
                {
                    obs_weak_source_release(weak_sidechain);
                    obs_source_release(sidechain);
                    weak_sidechain = nullptr;
                    sidechain = nullptr;
                    break;
                }
            }

            asmd->sidechain_update_mutex.lock();

            if (source.sidechain_name && strcmp(source.sidechain_name, new_name) == 0)
            {
                source.weak_sidechain = weak_sidechain;
                weak_sidechain = nullptr;
            }

            asmd->sidechain_update_mutex.unlock();

            if (sidechain)
            {
                source.source = sidechain;
                obs_source_add_audio_capture_callback(sidechain, asmd_capture, asmd);

                obs_weak_source_release(weak_sidechain);
                obs_source_release(sidechain);
            }

            bfree(new_name);
        }
    }

    if (!asmd->doRawCallback.load(std::memory_order_acquire))
    {
        for (auto& source : *asmd->sources)
        {
            asmd->sidechain_update_mutex.lock();
            auto* sourceRef = obs_weak_source_get_source(source.weak_sidechain);
            if (sourceRef)
            {
                obs_source_release(sourceRef);
                asmd->sidechain_update_mutex.unlock();
                return;
            }
            asmd->sidechain_update_mutex.unlock();
        }
        asmd->doRawCallback.store(true, std::memory_order_release);
    }
}

struct obs_source_info source_mixer = {
    .id = SOURCE_ID,
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = asmd_getname,
    .create = asmd_create,
    .destroy = asmd_destroy,
    .get_defaults = compressor_defaults,
    .get_properties = properties,
    .update = update,
    .video_tick = asmd_tick,
    .icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT,
};