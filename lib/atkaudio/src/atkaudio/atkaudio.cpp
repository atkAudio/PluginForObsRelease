#include "atkaudio.h"

#include <juce_audio_utils/juce_audio_utils.h>

void atk::create()
{
    juce::initialiseJuce_GUI();
}

void atk::pump()
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
}

void atk::destroy()
{
    juce::shutdownJuce_GUI();
}
