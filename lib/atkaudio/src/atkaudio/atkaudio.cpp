#include "atkaudio.h"

#include "UpdateCheck.h"

#include <juce_audio_utils/juce_audio_utils.h>
UpdateCheck* updateCheck = nullptr;

void atk::create()
{
    juce::initialiseJuce_GUI();
    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();
    updateCheck = new UpdateCheck(); // deleted at shutdown
}

void atk::pump()
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
}

void atk::destroy()
{
    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();
    juce::shutdownJuce_GUI();
}
