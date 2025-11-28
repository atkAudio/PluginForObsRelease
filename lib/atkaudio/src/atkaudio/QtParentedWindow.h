/*
 * QtParentedWindow.h
 *
 * A DocumentWindow base class that automatically parents JUCE windows
 * to the Qt main window (OBS). This prevents Direct2D rendering conflicts
 * between JUCE's VBlankThread and Qt's rendering loop on Windows.
 *
 * Usage: Replace `juce::DocumentWindow` with `atk::QtParentedDocumentWindow`
 */

#pragma once

#include "atkaudio.h"
#include "QtParentedWindowDebug.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace atk
{

/**
 * A DocumentWindow that automatically parents to the Qt main window.
 *
 * When running inside OBS (a Qt application), JUCE windows need to be parented
 * to Qt's main window to synchronize their rendering contexts. This prevents
 * Direct2D resource conflicts that can cause access violations during
 * asynchronous VBlank paint cycles.
 */
class QtParentedDocumentWindow : public juce::DocumentWindow
{
public:
    QtParentedDocumentWindow(
        const juce::String& name,
        juce::Colour backgroundColour,
        int requiredButtons = juce::DocumentWindow::allButtons,
        bool addToDesktopNow = true
    )
        : juce::DocumentWindow(
              name,
              backgroundColour,
              juce::DocumentWindow::allButtons,
              false
          ) // Never add to desktop in base ctor
    {
        juce::ignoreUnused(addToDesktopNow);
        (void)requiredButtons;
        DBG(
            "[QtParentedWindow] CTOR: " + name + ", addToDesktopNow=" + juce::String(addToDesktopNow ? "true" : "false")
        );
#if JUCE_LINUX
        // On Linux, NEVER add to desktop during construction.
        // X11 windows can auto-map and become unresponsive if the message loop
        // isn't fully running yet. Defer to first setVisible(true) call.
#else
        this->addToDesktop();
        this->setVisible(false); // Start hidden
#endif
    }

    ~QtParentedDocumentWindow() override = default;

    /**
     * Override setVisible to ensure addToDesktop is called through our override.
     */
    void setVisible(bool shouldBeVisible) override
    {
        if (shouldBeVisible && !isOnDesktop())
        {
            DBG("[QtParentedWindow] setVisible: Window not on desktop, calling addToDesktop first");
            addToDesktop();
        }
        juce::DocumentWindow::setVisible(shouldBeVisible);
    }

    /**
     * Override addToDesktop to parent to the Qt main window on Windows/macOS.
     * On Linux, we use standalone windows since Qt event loop integration
     * causes issues with X11 parenting.
     */
    void addToDesktop(int windowStyleFlags, void* nativeWindowToAttachTo = nullptr) override
    {
#if JUCE_LINUX
        // On Linux, don't parent to Qt - just use normal JUCE window behavior
        // This avoids conflicts with Qt's event loop when used as message pump
        juce::ignoreUnused(nativeWindowToAttachTo);
        DBG("[QtParentedWindow] addToDesktop (Linux): standalone window");
        juce::DocumentWindow::addToDesktop(windowStyleFlags, nullptr);
#else
        // On Windows/macOS, parent to Qt main window for D2D synchronization
        juce::ignoreUnused(nativeWindowToAttachTo);
        void* qtParent = atk::getQtMainWindowHandle();

        DBG("[QtParentedWindow] addToDesktop:");
        DBG("[QtParentedWindow]   Qt parent handle: " + juce::String::toHexString((juce::pointer_sized_int)qtParent));
        DBG("[QtParentedWindow]   styleFlags: " + juce::String::toHexString(windowStyleFlags));

        juce::DocumentWindow::addToDesktop(windowStyleFlags, qtParent);

        // Verify the window was created correctly
        if (auto* peer = getPeer())
        {
            void* nativeHandle = peer->getNativeHandle();
            DBG("[QtParentedWindow]   Created JUCE window handle: "
                + juce::String::toHexString((juce::pointer_sized_int)nativeHandle));
            DBG("[QtParentedWindow]   isOnDesktop: " + juce::String(isOnDesktop() ? "true" : "false"));

            // Log detailed platform-specific parenting info
            atk::logWindowParentingInfo(nativeHandle, qtParent);
        }
        else
        {
            DBG("[QtParentedWindow]   WARNING: No peer created!");
        }
#endif
    }

    // Convenience overload for addToDesktop with no args
    void addToDesktop()
    {
        addToDesktop(getDesktopWindowStyleFlags(), nullptr);
    }
};

} // namespace atk
