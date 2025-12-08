#pragma once

#include "atkaudio.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_events/juce_events.h>
#include <memory>
#include <string>

namespace atk
{

class atkAudioModule
{
public:
    atkAudioModule() = default;
    virtual ~atkAudioModule() = default;

    atkAudioModule(const atkAudioModule&) = delete;
    atkAudioModule& operator=(const atkAudioModule&) = delete;

    virtual void process(float** buffer, int numChannels, int numSamples, double sampleRate) = 0;
    virtual void getState(std::string& state) = 0;
    virtual void setState(std::string& state) = 0;

    virtual void setVisible(bool visible)
    {
        auto doUi = [this, visible]()
        {
            auto* window = getWindowComponent();
            if (!window)
                return;

            if (visible)
            {
                if (!window->isOnDesktop())
                {
                    if (auto* tlw = dynamic_cast<juce::TopLevelWindow*>(window))
                        tlw->addToDesktop();
                    else
                        window->addToDesktop(0);
                }

                window->setVisible(true);
                window->toFront(true);

                if (auto* docWindow = dynamic_cast<juce::DocumentWindow*>(window))
                    if (docWindow->isMinimised())
                        docWindow->setMinimised(false);
            }
            else
            {
                window->setVisible(false);
            }
        };

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            doUi();
        else
            juce::MessageManager::callAsync(doUi);
    }

    template <typename DestroyFunc>
    static void destroyOnMessageThread(DestroyFunc&& destroyer, int timeoutMs = 200)
    {
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        if (mm == nullptr)
        {
            destroyer();
            return;
        }

        if (mm->isThisTheMessageThread())
        {
            destroyer();
        }
        else
        {
            auto completionEvent = std::make_shared<juce::WaitableEvent>(true);
            mm->callAsync(
                [destroyer = std::forward<DestroyFunc>(destroyer), completionEvent]() mutable
                {
                    destroyer();
                    completionEvent->signal();
                }
            );
            completionEvent->wait(timeoutMs);
        }
    }

protected:
    virtual juce::Component* getWindowComponent() = 0;
};

} // namespace atk
