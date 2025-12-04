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

private:
    juce::File scannerPath;
    std::atomic<bool> shouldCancel{false};
    int timeoutMs = 30000;

    static juce::File findScannerExecutable();
    static void showMissingScannerWarning();
};

} // namespace atk
