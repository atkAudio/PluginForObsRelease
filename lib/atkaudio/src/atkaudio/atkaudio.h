#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

class QObject;

#define MAX_OBS_AUDIO_BUFFER_SIZE 1024

namespace atk
{
extern "C"
{
    void create();
    void destroy();
    void pump();
    void update();

    // Start message pump with Qt parent (typically Qt main window)
    // Must be called after create() and before any JUCE GUI operations
    void startMessagePump(QObject* qtParent);

    // Get Qt main window native handle for per-instance parent creation (fully lazy-initialized)
    void* getQtMainWindowHandle();

    // Helper to set window ownership for JUCE components after addToDesktop()
    void setWindowOwnership(juce::Component* component);

    // Apply colors to LookAndFeel from RGB values
    void applyColors(uint8_t bgR, uint8_t bgG, uint8_t bgB, uint8_t fgR, uint8_t fgG, uint8_t fgB);

    // Logging helper
    void logMessage(const juce::String& message);
}
} // namespace atk
