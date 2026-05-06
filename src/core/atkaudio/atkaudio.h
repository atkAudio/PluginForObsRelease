#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

class QObject;

#define MAX_OBS_AUDIO_BUFFER_SIZE 1024

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

// Return the settings file for the given name under the OBS config dir (or the JUCE default path).
juce::File getSettingsFile(const juce::String& name);

} // namespace atk
