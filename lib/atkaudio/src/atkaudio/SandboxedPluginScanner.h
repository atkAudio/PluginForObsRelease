#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace atk
{

/**
 * Out-of-process plugin scanner for JUCE's KnownPluginList.
 *
 * Scans plugins in a separate process to prevent crashes from taking down OBS.
 * If a plugin crashes during scanning, only the scanner process dies - OBS
 * continues running and moves on to the next plugin.
 *
 * The scanner executable (atkaudio-pluginforobs_scanner) must be installed
 * next to the main plugin library (.dll/.dylib/.so).
 *
 * Usage:
 *   knownPluginList.setCustomScanner(
 *       std::make_unique<atk::SandboxedScanner>()
 *   );
 */
class SandboxedScanner : public juce::KnownPluginList::CustomScanner
{
public:
    SandboxedScanner();
    ~SandboxedScanner() override;

    bool findPluginTypesFor(
        juce::AudioPluginFormat& format,
        juce::OwnedArray<juce::PluginDescription>& result,
        const juce::String& fileOrIdentifier
    ) override;

    void scanFinished() override;

    /** Check if the scanner executable is available. */
    bool isScannerAvailable() const
    {
        return scannerPath.existsAsFile();
    }

private:
    juce::File scannerPath;
    std::atomic<bool> shouldCancel{false};
    int timeoutMs = 30000;

    static juce::File findScannerExecutable();
    static void showMissingScannerWarning();
};

} // namespace atk
