#include "PluginHost2Settings.h"

#include <atkaudio/Logging.h>
#include <atkaudio/atkaudio.h>

PluginHost2Settings::PluginHost2Settings()
    : PluginHost2Settings(atk::getSettingsFile("atkAudio PluginHost2"))
{
}

PluginHost2Settings::PluginHost2Settings(const juce::File& file)
{
    juce::PropertiesFile::Options options;
    options.processLock = &processLock;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    properties = std::make_unique<juce::PropertiesFile>(file, options);
}

bool PluginHost2Settings::saveWindowState(juce::ResizableWindow& window, bool flush)
{
    if (window.isShowing() && !window.isMinimised())
        properties->setValue("mainWindowPos", window.getWindowStateAsString());
    if (flush && !properties->saveIfNeeded())
    {
        atk::logging::error(
            "PluginHost2Settings",
            "Couldn't save window settings to " + properties->getFile().getFullPathName()
        );
        return false;
    }
    return true;
}
