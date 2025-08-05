#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#define MAX_OBS_AUDIO_BUFFER_SIZE 1024

namespace atk
{
extern "C"
{
    void create();
    void destroy();
    void pump();
    void update();
}
} // namespace atk
