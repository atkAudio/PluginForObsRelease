#include "atkaudio.h"

#include <juce_audio_utils/juce_audio_utils.h>

DLL_EXPORT void atk::create()
{
    juce::initialiseJuce_GUI();
    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();
}

DLL_EXPORT void atk::destroy()
{
    juce::shutdownJuce_GUI();
}
