#include "atkaudio.h"

#include <juce_audio_utils/juce_audio_utils.h>

void atk::create()
{
    juce::initialiseJuce_GUI();
    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();
}

void atk::destroy()
{
    juce::shutdownJuce_GUI();
}
