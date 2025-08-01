#include "atkaudio.h"

#include "JuceApp.h"
START_JUCE_APPLICATION(Application)
#include "UpdateCheck.h"

#include <juce_audio_utils/juce_audio_utils.h>
UpdateCheck* updateCheck = nullptr;

void atk::create()
{
#ifdef JUCE_WINDOWS
    juce::JUCEApplicationBase::createInstance = []() -> juce::JUCEApplicationBase* { return new Application(); };
#endif
#ifndef NO_MESSAGE_PUMP
    juce::initialiseJuce_GUI();
    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();
#endif
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

void atk::update()
{
#ifndef JUCE_WINDOWS
    juce::JUCEApplicationBase::createInstance = []() -> juce::JUCEApplicationBase* { return new Application(); };
#endif

    if (updateCheck == nullptr)
        updateCheck = new UpdateCheck(); // deleted at shutdown
}