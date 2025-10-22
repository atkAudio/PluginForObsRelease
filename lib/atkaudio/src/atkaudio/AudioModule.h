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

    virtual ~atkAudioModule()
    {
        // Clean up parent component on message thread
        if (parentComponent)
        {
            auto* parent = parentComponent.release();
            juce::MessageManager::callAsync([parent] { delete parent; });
        }
    }

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

            // Lazy initialization - add to desktop on first show
            if (visible && !window->isOnDesktop())
            {
                ensureParentComponent();

                void* parentHandle = getParentNativeHandle();
                if (parentHandle)
                    window->addToDesktop(0, parentHandle);
                else
                    window->addToDesktop(0);

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

    /**
     * Get the native parent handle for this module instance
     * Can be used by derived classes for auxiliary windows (e.g., ChannelClient's matrix/settings windows)
     * Returns the peer handle of the parent component if available
     */
    void* getParentNativeHandle()
    {
        ensureParentComponent();
        if (parentComponent && parentComponent->getPeer())
            return parentComponent->getPeer()->getNativeHandle();
        return nullptr;
    }

private:
    /**
     * Ensure parent component exists for this module instance
     */
    void ensureParentComponent()
    {
        if (!parentComponent)
        {
            parentComponent = std::make_unique<juce::Component>();
            parentComponent->setVisible(false);

            // Get native window handle from OBS/Qt
            void* nativeHandle = atk::getQtMainWindowHandle();
            if (nativeHandle)
                parentComponent->addToDesktop(0, nativeHandle);
            else
                parentComponent->addToDesktop(0);
        }
    }

    // Per-instance parent component for window ownership
    // Each module has its own parent to avoid shared state and X11 errors
    std::unique_ptr<juce::Component> parentComponent;
};

} // namespace atk
