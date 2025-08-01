#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

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
