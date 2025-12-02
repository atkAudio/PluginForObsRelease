#include "SandboxedPluginScanner.h"

namespace atk
{

// Static flag to show warning only once per plugin lifetime
static bool scannerWarningShown = false;

juce::File SandboxedScanner::findScannerExecutable()
{
    auto pluginDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto scannerFileName = std::string("atkaudio-pluginforobs_scanner");
#if JUCE_WINDOWS
    scannerFileName += ".exe";
#endif
    return pluginDir.getChildFile(scannerFileName);
}

void SandboxedScanner::showMissingScannerWarning()
{
    // Only show warning once per plugin lifetime
    if (scannerWarningShown)
        return;
    scannerWarningShown = true;

    juce::MessageManager::callAsync(
        []()
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Warning",
                "Plugin scanner not found. Falling back to in-process scanning.",
                "OK"
            );
        }
    );
}

SandboxedScanner::SandboxedScanner()
    : scannerPath(findScannerExecutable())
{
    if (!scannerPath.existsAsFile())
    {
        DBG("[SandboxedScanner] Scanner not found: " + scannerPath.getFullPathName());
        showMissingScannerWarning();
    }
}

SandboxedScanner::~SandboxedScanner()
{
    shouldCancel.store(true);
}

bool SandboxedScanner::findPluginTypesFor(
    juce::AudioPluginFormat& format,
    juce::OwnedArray<juce::PluginDescription>& result,
    const juce::String& fileOrIdentifier
)
{
    if (shouldCancel.load())
        return true;

    if (!format.fileMightContainThisPluginType(fileOrIdentifier))
        return true;

    if (!isScannerAvailable())
    {
        format.findAllTypesForFile(result, fileOrIdentifier);
        return true;
    }

    juce::ChildProcess process;
    juce::StringArray args{scannerPath.getFullPathName(), fileOrIdentifier};
    if (!process.start(args))
        return true;

    auto output = process.readAllProcessOutput();

    if (!process.waitForProcessToFinish(timeoutMs))
    {
        process.kill();
        return true;
    }

    if (process.getExitCode() != 0)
        return true;

    // Strip any text before XML (e.g., debug output)
    if (auto xmlStart = output.indexOf("<?xml"); xmlStart > 0)
        output = output.substring(xmlStart);

    auto xml = juce::parseXML(output);
    if (!xml || !xml->getBoolAttribute("success", false))
        return true;

    for (auto* item : xml->getChildIterator())
    {
        auto desc = std::make_unique<juce::PluginDescription>();
        if (desc->loadFromXml(*item) && desc->pluginFormatName == format.getName())
            result.add(desc.release());
    }

    return true;
}

void SandboxedScanner::scanFinished()
{
    // Reset cancel flag so future scans work
    shouldCancel.store(false);
}

} // namespace atk
