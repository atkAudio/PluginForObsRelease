#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

namespace atk
{

constexpr auto DPI_NORMAL = 96.0f;

extern "C" void create();
extern "C" void destroy();
extern "C" void pump();

} // namespace atk
