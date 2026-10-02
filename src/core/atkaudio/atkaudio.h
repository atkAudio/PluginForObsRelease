#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <iostream>

class QObject;

// Project-wide debug-print macro, decoupled from JUCE's own DBG (which is a no-op
// outside JUCE_DEBUG, and otherwise routes through OutputDebugString rather than
// std::cerr). Enabled in local Debug/RelWithDebInfo builds only
// (see cmake/common/target_policies.cmake).
#if defined(ATK_DEBUG) && !defined(ATK_CI_BUILD)
#define ATK_DBG(x) std::cerr << x << std::endl
#else
#define ATK_DBG(x)
#endif

namespace atk
{
extern "C"
{
    bool create();
    void destroy();
    void pump();
    void update();

    // Start message pump with Qt parent (typically Qt main window)
    // Must be called after create() and before any JUCE GUI operations
    bool startMessagePump(QObject* qtParent);

    // Returns true when the OBS JUCE format lifecycle is fully initialized.
    bool isReady();

    // Returns true while lifecycle shutdown is in progress.
    bool isShuttingDown();

    // Get Qt main window native handle for per-instance parent creation (fully lazy-initialized)
    void* getQtMainWindowHandle();

    // Helper to set window ownership for JUCE components after addToDesktop()
    void setWindowOwnership(juce::Component* component);

    // Apply colors to LookAndFeel from RGB values
    void applyColors(uint8_t bgR, uint8_t bgG, uint8_t bgB, uint8_t fgR, uint8_t fgG, uint8_t fgB);

    // Logging helper
    void logMessage(const juce::String& message);
}

// Return the settings file for the given name under the OBS config dir. This requires a live OBS module context and
// intentionally has no fallback path.
juce::File getSettingsFile(const juce::String& name);

} // namespace atk
