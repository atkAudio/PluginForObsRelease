#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

class PluginScannerSubprocess final
    : private ChildProcessWorker
    , private AsyncUpdater
{
public:
    PluginScannerSubprocess()
    {
        juce::addDefaultFormatsToManager(formatManager);
    }

    using ChildProcessWorker::initialiseFromCommandLine;

private:
    void handleMessageFromCoordinator(const MemoryBlock& mb) override
    {
        if (mb.isEmpty())
            return;

        const std::lock_guard<std::mutex> lock(mutex);

        if (const auto results = doScan(mb); !results.isEmpty())
        {
            sendResults(results);
        }
        else
        {
            pendingBlocks.emplace(mb);
            triggerAsyncUpdate();
        }
    }

    void handleConnectionLost() override
    {
    }

    void handleAsyncUpdate() override
    {
        for (;;)
        {
            const std::lock_guard<std::mutex> lock(mutex);

            if (pendingBlocks.empty())
                return;

            sendResults(doScan(pendingBlocks.front()));
            pendingBlocks.pop();
        }
    }

    OwnedArray<PluginDescription> doScan(const MemoryBlock& block)
    {
        MemoryInputStream stream{block, false};
        const auto formatName = stream.readString();
        const auto identifier = stream.readString();

        PluginDescription pd;
        pd.fileOrIdentifier = identifier;
        pd.uniqueId = pd.deprecatedUid = 0;

        const auto matchingFormat = [&]() -> AudioPluginFormat*
        {
            for (auto* format : formatManager.getFormats())
                if (format->getName() == formatName)
                    return format;

            return nullptr;
        }();

        OwnedArray<PluginDescription> results;

        if (matchingFormat != nullptr
            && (MessageManager::getInstance()->isThisTheMessageThread()
                || matchingFormat->requiresUnblockedMessageThreadDuringCreation(pd)))
        {
            matchingFormat->findAllTypesForFile(results, identifier);
        }

        return results;
    }

    void sendResults(const OwnedArray<PluginDescription>& results)
    {
        XmlElement xml("LIST");

        for (const auto& desc : results)
            xml.addChildElement(desc->createXml().release());

        const auto str = xml.toString();
        sendMessageToCoordinator({str.toRawUTF8(), str.getNumBytesAsUTF8()});
    }

    std::mutex mutex;
    std::queue<MemoryBlock> pendingBlocks;
    AudioPluginFormatManager formatManager;
};
