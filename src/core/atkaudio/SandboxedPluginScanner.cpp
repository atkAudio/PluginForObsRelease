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
    {
        // Process failed to start - track as failed scan
        failedScans.push_back({fileOrIdentifier, format.getName()});
        DBG("[SandboxedScanner] Failed to start scanner for: " + fileOrIdentifier);
        return true;
    }

    auto output = process.readAllProcessOutput();

    if (!process.waitForProcessToFinish(timeoutMs))
    {
        process.kill();
        // Timeout - track as failed scan
        failedScans.push_back({fileOrIdentifier, format.getName()});
        DBG("[SandboxedScanner] Scanner timeout for: " + fileOrIdentifier);
        return true;
    }

    if (process.getExitCode() != 0)
    {
        // Non-zero exit code - track as failed scan
        failedScans.push_back({fileOrIdentifier, format.getName()});
        DBG(
            "[SandboxedScanner] Scanner exit code " + juce::String(process.getExitCode()) + " for: " + fileOrIdentifier
        );
        return true;
    }

    // Strip any text before XML (e.g., debug output)
    if (auto xmlStart = output.indexOf("<?xml"); xmlStart > 0)
        output = output.substring(xmlStart);

    auto xml = juce::parseXML(output);
    if (!xml || !xml->getBoolAttribute("success", false))
    {
        // Failed to parse or scan reported failure - track as failed scan
        failedScans.push_back({fileOrIdentifier, format.getName()});
        DBG("[SandboxedScanner] Scan failed for: " + fileOrIdentifier);
        return true;
    }

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

    // If there were failed scans, offer fallback option
    if (!failedScans.empty())
        offerFallbackScan();
}

void SandboxedScanner::offerFallbackScan()
{
    auto numFailed = failedScans.size();

    // Build list of failed plugin names
    juce::String failedList;
    for (size_t i = 0; i < std::min(numFailed, size_t(5)); ++i)
        failedList += juce::File(failedScans[i].fileOrIdentifier).getFileName() + "\n";
    if (numFailed > 5)
        failedList += "...and " + juce::String(numFailed - 5) + " more\n";

    juce::String message = juce::String(numFailed)
                         + " plugin(s) failed scan:\n\n"
                         + failedList
                         + "\nRetry scan in-process? May succeed or crash/hang.";

    auto failedScansCopy = failedScans;
    auto* fmCopy = formatManager;
    auto* listCopy = knownPluginList;
    failedScans.clear();

    // Delay showing dialog by 1 second to ensure JUCE's dialog appears first
    juce::Timer::callAfterDelay(
        1000,
        [message, failedScansCopy, fmCopy, listCopy]()
        {
            auto options = juce::MessageBoxOptions()
                               .withIconType(juce::MessageBoxIconType::QuestionIcon)
                               .withTitle("Out-of-process scan failed")
                               .withMessage(message)
                               .withButton("Retry")
                               .withButton("Skip");

            juce::AlertWindow::showAsync(
                options,
                [failedScansCopy, fmCopy, listCopy](int result)
                {
                    if (result != 1 || !fmCopy || !listCopy)
                        return;

                    // Dismiss JUCE's "Scan complete" dialog before starting fallback scan
                    for (int i = juce::ModalComponentManager::getInstance()->getNumModalComponents(); --i >= 0;)
                    {
                        if (auto* alert = dynamic_cast<juce::AlertWindow*>(
                                juce::ModalComponentManager::getInstance()->getModalComponent(i)
                            ))
                        {
                            // JUCE's PluginListComponent shows dialog with title "Scan complete"
                            if (alert->getName() == "Scan complete" || alert->getName() == TRANS("Scan complete"))
                                alert->exitModalState(0);
                        }
                    }

                    for (const auto& failed : failedScansCopy)
                    {
                        auto* format = [&]() -> juce::AudioPluginFormat*
                        {
                            for (int i = 0; i < fmCopy->getNumFormats(); ++i)
                                if (fmCopy->getFormat(i)->getName() == failed.formatName)
                                    return fmCopy->getFormat(i);
                            return nullptr;
                        }();

                        if (!format)
                            continue;

                        juce::OwnedArray<juce::PluginDescription> descriptions;
                        format->findAllTypesForFile(descriptions, failed.fileOrIdentifier);

                        for (auto* desc : descriptions)
                            if (desc)
                                listCopy->addType(*desc);
                    }
                }
            );
        }
    );
}

} // namespace atk
