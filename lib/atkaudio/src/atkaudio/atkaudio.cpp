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
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
}

void atk::destroy()
{
    // Explicitly delete UpdateCheck before shutting down JUCE
    if (updateCheck != nullptr)
    {
        delete updateCheck;
        updateCheck = nullptr;
    }

    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();

    // Force hide all visible JUCE components before shutdown
    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
    {
        if (auto* comp = desktop.getComponent(i))
        {
            if (comp->isVisible())
                comp->setVisible(false);
        }
    }

    // Process any pending messages after hiding components
    for (int i = 0; i < 100; ++i)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        if (juce::Desktop::getInstance().getNumComponents() == 0)
            break;
    }

    auto numComponents = juce::Desktop::getInstance().getNumComponents();
    DBG("shutting down with " << numComponents << " remaining components");

    juce::shutdownJuce_GUI();

    // Brief wait only if components remain - they should be mostly cleaned up by now
    if (numComponents > 0)
    {
        int waitMs = jmin(1000, 100 * numComponents); // Reduced from 200ms per component
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
    }
}

void atk::update()
{
#ifndef JUCE_WINDOWS
    juce::JUCEApplicationBase::createInstance = []() -> juce::JUCEApplicationBase* { return new Application(); };
#endif

    if (updateCheck == nullptr)
        updateCheck = new UpdateCheck(); // deleted at shutdown
}