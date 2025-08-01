#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

namespace atk
{

extern "C" void create();
extern "C" void destroy();
extern "C" void pump();
extern "C" void update();

} // namespace atk
