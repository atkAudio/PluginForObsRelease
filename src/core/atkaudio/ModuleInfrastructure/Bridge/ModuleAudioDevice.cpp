#include "ModuleAudioDevice.h"

#include <obs.h>
#include <media-io/audio-io.h>

namespace atk
{

int getOBSAudioFrameSize()
{
    return AUDIO_OUTPUT_FRAMES; // Actual OBS constant
}

ModuleOBSAudioDevice::ModuleOBSAudioDevice(
    const juce::String& deviceName,
    std::shared_ptr<ModuleDeviceCoordinator> deviceCoordinator,
    const juce::String& typeName
)
    : juce::AudioIODevice(deviceName, typeName)
    , coordinator(deviceCoordinator)
{
    // Get current OBS audio configuration
    auto* obsAudio = obs_get_audio();
    if (obsAudio)
    {
        obsChannelCount = audio_output_get_channels(obsAudio);
        obsSampleRate = audio_output_get_sample_rate(obsAudio);
    }
}

ModuleOBSAudioDevice::~ModuleOBSAudioDevice()
{
    close();
}

} // namespace atk
