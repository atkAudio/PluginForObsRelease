#pragma once

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
        options.applicationName = "atkAudio Shared";
        options.filenameSuffix = "settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = "atkAudio Plugin";
        options.processLock = &fileLock;
        appProperties.setStorageParameters(options);
    }

    ~SharedPluginList() override
    {
        clearSingletonInstance();
    }

    juce::PropertiesFile* getPropertiesFile()
    {
        return appProperties.getUserSettings();
    }

    void loadPluginList(juce::KnownPluginList& list, bool excludeInternalPlugins = false)
    {
        const juce::ScopedLock sl(lock);
        appProperties.getUserSettings()->reload();

        auto saved = appProperties.getUserSettings()->getXmlValue("pluginList");
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
            appProperties.getUserSettings()->setValue("pluginList", xml.get());
            appProperties.saveIfNeeded();
        }
    }

    JUCE_DECLARE_SINGLETON_SINGLETHREADED(SharedPluginList, false)

private:
    juce::InterProcessLock fileLock;
    juce::CriticalSection lock;
    juce::ApplicationProperties appProperties;
};

} // namespace atk
