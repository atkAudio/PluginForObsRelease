#pragma once

#include "atkaudio.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace atk
{

class SharedPluginList : public juce::DeletedAtShutdown
{
public:
    SharedPluginList()
        : fileLock("atkAudioSharedPluginList")
    {
        juce::PropertiesFile::Options options;
        options.processLock = &fileLock;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        propertiesFile = std::make_unique<juce::PropertiesFile>(atk::getSettingsFile("atkAudio Shared"), options);
    }

    ~SharedPluginList() override
    {
        clearSingletonInstance();
    }

    juce::PropertiesFile* getPropertiesFile()
    {
        return propertiesFile.get();
    }

    void loadPluginList(juce::KnownPluginList& list, bool excludeInternalPlugins = false)
    {
        const juce::ScopedLock sl(lock);
        propertiesFile->reload();

        auto saved = propertiesFile->getXmlValue("pluginList");
        if (!saved)
            return;

        if (!excludeInternalPlugins)
        {
            list.recreateFromXml(*saved);
            return;
        }

        juce::KnownPluginList fullList;
        fullList.recreateFromXml(*saved);
        for (const auto& type : fullList.getTypes())
            if (type.pluginFormatName != "Internal")
                list.addType(type);
    }

    void savePluginList(const juce::KnownPluginList& list)
    {
        const juce::ScopedLock sl(lock);
        if (auto xml = list.createXml())
        {
            propertiesFile->setValue("pluginList", xml.get());
            propertiesFile->saveIfNeeded();
        }
    }

    JUCE_DECLARE_SINGLETON_SINGLETHREADED(SharedPluginList, false)

private:
    juce::InterProcessLock fileLock;
    juce::CriticalSection lock;
    std::unique_ptr<juce::PropertiesFile> propertiesFile;
};

} // namespace atk
