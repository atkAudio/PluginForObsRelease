#pragma once

#include "atkaudio.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include <string>

namespace atk
{

/**
 * Unified base class for all atkaudio modules
 * Combines audio processing and window management
 * Each module instance has its own parent component for proper window isolation
 *
 * Window Lifecycle:
 * - Windows are created lazily on first setVisible(true)
 * - Windows are destroyed at module destruction
 */
class atkAudioModule
{
public:
    atkAudioModule() = default;
    virtual ~atkAudioModule() = default;

    // Disable copy
    atkAudioModule(const atkAudioModule&) = delete;
    atkAudioModule& operator=(const atkAudioModule&) = delete;

    /**
     * Process audio buffer
     * @param buffer Audio buffer (interleaved channels)
     * @param numChannels Number of audio channels
     * @param numSamples Number of samples per channel
     * @param sampleRate Sample rate in Hz
     */
    virtual void process(float** buffer, int numChannels, int numSamples, double sampleRate) = 0;

    /**
     * Get module state as string (for saving)
     */
    virtual void getState(std::string& state) = 0;

    /**
     * Set module state from string (for loading)
     */
    virtual void setState(std::string& state) = 0;

    /**
     * Set window visibility - handles safe threading
     * Window is created on first show and added to desktop
     */
    void setVisible(bool visible)
    {
        auto doUi = [this, visible]()
        {
            auto* window = getWindowComponent();
            if (!window)
                return;

            // If window is already on desktop, just toggle visibility
            if (window->isOnDesktop())
            {
                window->setVisible(visible);
                if (visible)
                    window->toFront(true);
                return;
            }

            // Lazy initialization - add to desktop on first show
            if (visible)
            {
                // DocumentWindow (TopLevelWindow) requires calling through TopLevelWindow interface
                if (auto* topLevel = dynamic_cast<juce::TopLevelWindow*>(window))
                {
                    topLevel->addToDesktop(); // TopLevelWindow has no-args version
                }
                else
                {
                    // Regular components can use flags
                    window->addToDesktop(juce::ComponentPeer::windowAppearsOnTaskbar);
                }

                // Center the window on screen
                if (auto* docWindow = dynamic_cast<juce::DocumentWindow*>(window))
                    docWindow->centreWithSize(docWindow->getWidth(), docWindow->getHeight());
                else
                    window->centreWithSize(window->getWidth(), window->getHeight());
            }

            window->setVisible(visible);

            if (visible)
            {
                window->toFront(true);

                // Handle minimised state if the window supports it
                if (auto* docWindow = dynamic_cast<juce::DocumentWindow*>(window))
                {
                    if (docWindow->isMinimised())
                        docWindow->setMinimised(false);
                }
            }
        };

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            doUi();
        else
            juce::MessageManager::callAsync(doUi);
    }

protected:
    /**
     * Get the main window component for this module
     * Derived classes must implement this to return their window
     * The window is created by derived class (can be lazy or in constructor)
     */
    virtual juce::Component* getWindowComponent() = 0;
};

} // namespace atk
