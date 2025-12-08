#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace atk
{

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

    bool isScannerAvailable() const
    {
        return scannerPath.existsAsFile();
    }

    // Set the format manager to use for fallback scanning
    void setFormatManager(juce::AudioPluginFormatManager* manager)
    {
        formatManager = manager;
    }

    // Set the known plugin list for adding fallback-scanned plugins
    void setKnownPluginList(juce::KnownPluginList* list)
    {
        knownPluginList = list;
    }

private:
    juce::File scannerPath;
    std::atomic<bool> shouldCancel{false};
    int timeoutMs = 30000;

    // Track failed plugin scans for fallback option
    struct FailedScan
    {
        juce::String fileOrIdentifier;
        juce::String formatName;
    };

    std::vector<FailedScan> failedScans;

    // References for fallback scanning
    juce::AudioPluginFormatManager* formatManager = nullptr;
    juce::KnownPluginList* knownPluginList = nullptr;

    static juce::File findScannerExecutable();
    static void showMissingScannerWarning();
    void offerFallbackScan();
};

} // namespace atk
