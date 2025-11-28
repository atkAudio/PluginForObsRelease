#include "config.h"

#include <algorithm>
#include <atkaudio/FifoBuffer2.h>
#include <deque>
#include <iostream>
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
#define S_POSTFADER "post_fader"
#define TEXT_POSTFADER MT_("Post-Fader")
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

    float gain;
    bool postMute;
    bool postFader;

    SyncBuffer syncBuffer;
    std::vector<std::vector<float>> writeBuffer;
    std::vector<float*> writePtrs;
    std::vector<std::vector<float>> readBuffer;
    std::vector<float*> readPtrs;

    std::atomic_bool isActive = false;
    std::atomic_uint64_t last_callback_time = 0;

    int calls_since_empty = 0; // Track how many calls since buffer was empty
};

struct audiosourcemixer_data
{
    std::unique_ptr<std::deque<source_data>> sources;

    obs_source_t* source;
    obs_properties_t* properties = nullptr;

    obs_source_audio audio_data;

    std::mutex sidechain_update_mutex;
    std::mutex captureCallbackMutex;

    std::atomic_int speaker_layout = 0;

    std::vector<std::vector<float>> tempBuffer;
};

static void audio_output_callback(void* param, size_t mix_idx, struct audio_data* data)
{
    UNUSED_PARAMETER(mix_idx);
    UNUSED_PARAMETER(data);

    struct audiosourcemixer_data* asmd = (struct audiosourcemixer_data*)param;

    // Check if we have any configured sources
    bool hasConfiguredSources = false;
    if (asmd && asmd->sources)
    {
        for (const auto& src : *asmd->sources)
        {
            if (src.sidechain_name && *src.sidechain_name)
            {
                hasConfiguredSources = true;
                break;
            }
        }
    }

    // Only produce silence if no sources are configured
    if (!hasConfiguredSources && asmd)
    {
        auto numChannels = (int)audio_output_get_channels(obs_get_audio());
        auto sampleRate = audio_output_get_sample_rate(obs_get_audio());
        const int frames = (int)data->frames;

        // Prepare output buffer with silence
        if (asmd->tempBuffer.size() < numChannels)
            asmd->tempBuffer.resize(numChannels);

        for (int i = 0; i < numChannels; i++)
        {
            if (asmd->tempBuffer[i].size() < frames)
                asmd->tempBuffer[i].resize(frames, 0.0f);
            std::fill_n(asmd->tempBuffer[i].data(), frames, 0.0f);
            asmd->audio_data.data[i] = (const uint8_t*)asmd->tempBuffer[i].data();
        }

        asmd->audio_data.speakers =
            (speaker_layout)(asmd->speaker_layout.load() == 0 ? numChannels : asmd->speaker_layout.load());
        asmd->audio_data.samples_per_sec = sampleRate;
        asmd->audio_data.format = AUDIO_FORMAT_FLOAT_PLANAR;
        asmd->audio_data.frames = frames;
        asmd->audio_data.timestamp = os_gettime_ns();

        obs_source_output_audio(asmd->source, &asmd->audio_data);
    }
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
    constexpr auto fifoSize = 1024 * 1024;
    struct audiosourcemixer_data* asmd = (struct audiosourcemixer_data*)param;
    auto numChannels = (int)audio_output_get_channels(obs_get_audio());
    auto sampleRate = audio_output_get_sample_rate(obs_get_audio());
    auto frames = (int)audio_data->frames;

    // Find the source that triggered this callback
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

    // Handle muted sources
    source->isActive.store(false, std::memory_order_release);
    if (muted && source->postMute)
    {
        source->syncBuffer.reset();
        return;
    }
    source->isActive.store(true, std::memory_order_release);

    auto currentTime = os_gettime_ns();
    auto lastCallbackTime = source->last_callback_time.load(std::memory_order_acquire);
    if (lastCallbackTime > 0 && (currentTime - lastCallbackTime) > 3000000000)
        source->syncBuffer.reset();

    source->last_callback_time.store(currentTime, std::memory_order_release);

    // Ensure write buffers are sized
    if (source->writeBuffer.size() < numChannels)
    {
        source->writeBuffer.resize(numChannels);
        source->writePtrs.resize(numChannels);
    }
    for (int i = 0; i < numChannels; i++)
    {
        if (source->writeBuffer[i].size() < frames)
            source->writeBuffer[i].resize(frames);
        source->writePtrs[i] = source->writeBuffer[i].data();
    }

    // Apply gain and write to SyncBuffer
    std::scoped_lock lock(asmd->captureCallbackMutex);

    float totalGain = source->gain;
    if (source->postFader && sourceIn)
        totalGain *= obs_source_get_volume(sourceIn);

    // Apply gain to write buffer
    for (int i = 0; i < numChannels; i++)
    {
        const float* sourcePtr = (const float*)audio_data->data[i];
        float* writePtr = source->writeBuffer[i].data();

        for (int j = 0; j < frames; j++)
            writePtr[j] = sourcePtr[j] * totalGain;
    }

    if (!source->syncBuffer.getIsPrepared())
    {
        source->syncBuffer.setTargetLevelFactor(1.0);
        source->syncBuffer.setInterpolationType(atk::InterpolationType::Linear);
        source->syncBuffer.prepare(numChannels, frames, sampleRate);
    }

    // Write to SyncBuffer with source sample rate
    source->syncBuffer.write(source->writePtrs.data(), numChannels, frames, sampleRate);

    // Check all sources: find minimum ready samples, return early if any active source has no data
    int minReadySamples = frames;
    int activeSourceCount = 0;
    for (auto& src : *asmd->sources)
    {
        std::scoped_lock lock(asmd->sidechain_update_mutex);
        auto sourceRef = obs_weak_source_get_source(src.weak_sidechain);
        if (!sourceRef)
        {
            src.syncBuffer.reset();
            src.isActive.store(false, std::memory_order_release);
        }
        obs_source_release(sourceRef);

        if (!src.isActive.load(std::memory_order_acquire))
            continue;

        // Check for timeout based on last callback time
        auto prevSrcTime = src.last_callback_time.load(std::memory_order_acquire);
        if (currentTime > prevSrcTime && currentTime - prevSrcTime > 3000000000)
        {
            src.syncBuffer.reset();
            src.isActive.store(false, std::memory_order_release);
            continue;
        }

        auto numReady = src.syncBuffer.getNumReady();

        // Find minimum samples across all sources
        if (numReady < minReadySamples)
            minReadySamples = numReady;

        activeSourceCount++;
    }

    // If no active sources remain, don't output
    if (activeSourceCount == 0 || minReadySamples <= 0)
        return;

    // Prepare output buffer
    if (asmd->tempBuffer.size() < numChannels)
        asmd->tempBuffer.resize(numChannels);

    for (int i = 0; i < numChannels; i++)
    {
        if (asmd->tempBuffer[i].size() < minReadySamples)
            asmd->tempBuffer[i].resize(minReadySamples);
        std::fill_n(asmd->tempBuffer[i].data(), minReadySamples, 0.0f);
        asmd->audio_data.data[i] = (const uint8_t*)asmd->tempBuffer[i].data();
    }

    // Set up output metadata
    asmd->audio_data.speakers =
        (speaker_layout)(asmd->speaker_layout.load() == 0 ? numChannels : asmd->speaker_layout.load());
    asmd->audio_data.samples_per_sec = sampleRate;
    asmd->audio_data.format = AUDIO_FORMAT_FLOAT_PLANAR;
    asmd->audio_data.frames = minReadySamples;

    // Use current system time for timestamp
    asmd->audio_data.timestamp = os_gettime_ns();

    // Mix all sources into output
    for (auto& src : *asmd->sources)
    {
        std::scoped_lock lock(asmd->sidechain_update_mutex);
        if (!src.weak_sidechain || !src.isActive.load(std::memory_order_acquire))
            continue;

        // Ensure read buffers are sized
        if (src.readBuffer.size() < numChannels)
        {
            src.readBuffer.resize(numChannels);
            src.readPtrs.resize(numChannels);
        }
        for (int i = 0; i < numChannels; i++)
        {
            if (src.readBuffer[i].size() < minReadySamples)
                src.readBuffer[i].resize(minReadySamples);
            // Clear buffer before reading
            std::fill_n(src.readBuffer[i].data(), minReadySamples, 0.0f);
            src.readPtrs[i] = src.readBuffer[i].data();
        }

        // Read from SyncBuffer (addToBuffer=false since we cleared)
        if (src.syncBuffer.read(src.readPtrs.data(), numChannels, minReadySamples, sampleRate))
        {
            // Add to output buffer
            for (int i = 0; i < numChannels; i++)
            {
                auto* targetPtr = (float*)asmd->audio_data.data[i];
                const float* readPtr = src.readBuffer[i].data();

                for (int j = 0; j < minReadySamples; j++)
                    targetPtr[j] += readPtr[j];
            }
        }
    }

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

    return true;
}

static obs_properties_t* properties(void* data)
{
    struct audiosourcemixer_data* asmd = (struct audiosourcemixer_data*)data;

    // Recreate properties dynamically each time to reflect current source count (+1 extra slot)
    obs_properties_t* props = obs_properties_create();
    asmd->properties = props;

    auto* layout = obs_properties_add_list(props, S_LAYOUT, TEXT_LAYOUT, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

    obs_property_list_add_int(layout, TEXT_LAYOUT_DEFAULT, 0);
    obs_property_list_add_int(layout, TEXT_LAYOUT_MONO, 1);
    obs_property_list_add_int(layout, TEXT_LAYOUT_STEREO, 2);

    obs_source_t* parent = nullptr;
    if (asmd)
        parent = obs_filter_get_parent(asmd->source);

    // Count configured sources (those with valid sidechain names)
    size_t configured = 0;
    if (asmd && asmd->sources)
    {
        for (const auto& src : *asmd->sources)
            if (src.sidechain_name && *src.sidechain_name)
                configured++;
    }

    // Show minimum 8 slots, or configured+1 (one extra empty slot)
    const size_t minSlots = 8;
    const size_t totalSlots = std::max<size_t>(minSlots, configured + 1);

    for (size_t i = 0; i < totalSlots; ++i)
    {
        const int idx = (int)i + 1; // 1-based labels/keys

        std::string propText = S_SIDECHAIN_SOURCE;
        propText += std::to_string(idx);
        std::string textLabel = TEXT_SIDECHAIN_SOURCE;
        textLabel += " " + std::to_string(idx);
        obs_property_t* sources = obs_properties_add_list(
            props,
            propText.c_str(),
            textLabel.c_str(),
            OBS_COMBO_TYPE_LIST,
            OBS_COMBO_FORMAT_STRING
        );

        obs_property_list_add_string(sources, obs_module_text("None"), "none");

        propText = S_GAIN_DB;
        propText += std::to_string(idx);
        textLabel = TEXT_GAIN_DB;
        textLabel += " " + std::to_string(idx);
        obs_property_t* p =
            obs_properties_add_float_slider(props, propText.c_str(), textLabel.c_str(), -30.0, 30.0, 0.1);
        obs_property_float_set_suffix(p, " dB");

        propText = S_POSTMUTE;
        propText += std::to_string(idx);
        textLabel = TEXT_POSTMUTE;
        textLabel += " " + std::to_string(idx);
        obs_properties_add_bool(props, propText.c_str(), textLabel.c_str());

        propText = S_POSTFADER;
        propText += std::to_string(idx);
        textLabel = TEXT_POSTFADER;
        textLabel += " " + std::to_string(idx);
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

    // First pass: count how many sources are configured in the saved settings
    size_t configuredCount = 0;
    for (size_t i = 0; i < 1000; ++i) // reasonable upper limit
    {
        std::string sidechainText = S_SIDECHAIN_SOURCE;
        sidechainText += std::to_string(i + 1);
        const char* sidechain_name = obs_data_get_string(s, sidechainText.c_str());
        if (!sidechain_name || !*sidechain_name || strcmp(sidechain_name, "none") == 0)
            break;
        configuredCount = i + 1;
    }

    // Ensure we have at least 8 sources, and enough for all configured sources + 1 empty
    const size_t requiredSize = std::max<size_t>(8, configuredCount + 1);
    while (asmd->sources->size() < requiredSize)
    {
        asmd->sources->emplace_back();
        auto& newSrc = asmd->sources->back();
    }

    // Trim to maintain minimum 8 sources, or configuredCount+1 (one empty slot after configured)
    const size_t targetSize = std::max<size_t>(8, configuredCount + 1);
    while (asmd->sources->size() > targetSize)
        asmd->sources->pop_back();

    // Update existing sources from properties
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

        paramText = S_POSTFADER;
        paramText += std::to_string(i + 1);
        source.postFader = obs_data_get_bool(s, paramText.c_str());

        const char* sidechain_name = obs_data_get_string(s, sidechainText.c_str());

        bool valid_sidechain = sidechain_name && *sidechain_name && strcmp(sidechain_name, "none") != 0;
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

    // If the extra (last) slot is configured with a sidechain, append a new empty source
    {
        const int idx = (int)asmd->sources->size() + 1; // 1-based for property names
        std::string sidechainText = S_SIDECHAIN_SOURCE;
        sidechainText += std::to_string(idx);
        const char* sidechain_name = obs_data_get_string(s, sidechainText.c_str());
        const bool valid_sidechain = sidechain_name && *sidechain_name && strcmp(sidechain_name, "none") != 0;
        if (valid_sidechain)
        {
            asmd->sources->emplace_back();
            source_data& newSrc = asmd->sources->back();
            newSrc.source = nullptr;
            newSrc.weak_sidechain = nullptr;
            newSrc.sidechain_name = bstrdup(sidechain_name);
            newSrc.sidechain_check_time = os_gettime_ns() - 3000000000;
            newSrc.gain =
                obs_db_to_mul((float)obs_data_get_double(s, (std::string(S_GAIN_DB) + std::to_string(idx)).c_str()));
            newSrc.postMute = obs_data_get_bool(s, (std::string(S_POSTMUTE) + std::to_string(idx)).c_str());
            newSrc.postFader = obs_data_get_bool(s, (std::string(S_POSTFADER) + std::to_string(idx)).c_str());
        }
    }
}

static void* asmd_create(obs_data_t* settings, obs_source_t* source)
{
    auto* asmd = new audiosourcemixer_data();

    asmd->source = source;
    // Start with 8 empty sources; the UI will expose one extra after all are filled
    asmd->sources.reset(new std::deque<source_data>(8));

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

    UNUSED_PARAMETER(seconds);

    for (auto& source : *asmd->sources)
    {
        char* new_name = nullptr;
        asmd->sidechain_update_mutex.lock();

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