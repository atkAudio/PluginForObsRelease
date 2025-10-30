#pragma once
#include "LookAndFeel.h"

#include <config.h>
#include <juce_audio_utils/juce_audio_utils.h>

constexpr auto OWNER = "atkAudio";
constexpr auto DISPLAY_NAME = PLUGIN_DISPLAY_NAME;
constexpr auto REPO = "PluginForObsRelease";
constexpr auto VERSION = PLUGIN_VERSION;
constexpr auto JSON_VALUE = "tag_name";
constexpr auto FILENAME = "atkaudio-pluginforobs.zip";

class UpdateCheck
    : public juce::ModalComponentManager::Callback
    , public juce::DeletedAtShutdown
{
public:
    UpdateCheck(const juce::String& repoOwner = OWNER, const juce::String& repoName = REPO)
        : DeletedAtShutdown()
        , owner(repoOwner)
        , repo(repoName)
    {
        checkForUpdate();
    }

    juce::String getValueFromJson(const juce::String& jsonString, const juce::String& key)
    {
        juce::var json = juce::JSON::parse(jsonString);
        if (json.isObject())
        {
            auto jsonObject = json.getDynamicObject();
            if (jsonObject->hasProperty(key))
                return jsonObject->getProperty(key).toString();
        }
        return {};
    }

    bool isNewerVersionThanCurrent(
        const juce::String& remoteVersion, //
        const juce::String& localVersion = VERSION
    )
    {
        jassert(remoteVersion.isNotEmpty());

        auto remoteTokens = juce::StringArray::fromTokens(remoteVersion, ".", {});
        auto localTokens = juce::StringArray::fromTokens(localVersion, ".", {});

        if (remoteTokens[0].getIntValue() == localTokens[0].getIntValue())
        {
            if (remoteTokens[1].getIntValue() == localTokens[1].getIntValue())
                return remoteTokens[2].getIntValue() > localTokens[2].getIntValue();

            return remoteTokens[1].getIntValue() > localTokens[1].getIntValue();
        }

        return remoteTokens[0].getIntValue() > localTokens[0].getIntValue();
    }

    void checkForUpdate()
    {
        auto appDir =
            juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile(PLUGIN_DISPLAY_NAME);
        appDir.createDirectory();

        juce::File lastVersionFile = appDir.getChildFile("version_check");

        if (lastVersionFile.existsAsFile())
        {
            auto creationTime = lastVersionFile.getCreationTime();
            auto now = juce::Time::getCurrentTime();
            auto threeMonthsMs = (long long)3 * 30 * 24 * 60 * 60 * 1000;
            if (now.toMilliseconds() - creationTime.toMilliseconds() > threeMonthsMs)
                lastVersionFile.deleteFile();
        }

        if (!lastVersionFile.existsAsFile())
        {
            lastVersionFile.create();
        }
        else if (juce::Time::getCurrentTime().toMilliseconds()
                     - lastVersionFile.getLastModificationTime().toMilliseconds()
                 < 7 * 24 * 60 * 60 * 1000)
        {
            return;
        }

        // Past 7 days - do the check
        juce::URL versionURL("https://api.github.com/repos/" + owner + "/" + repo + "/releases/latest");

        std::unique_ptr<juce::InputStream> inStream(versionURL.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs(5000)
        ));

        if (inStream == nullptr)
            return;

        auto remoteVersionString = inStream->readEntireStreamAsString().trim();

        if (remoteVersionString.isEmpty())
            return;

        remoteVersionString = getValueFromJson(remoteVersionString, JSON_VALUE);

        latestRemoteVersion = remoteVersionString;

        // Update mod time now that we've checked
        lastVersionFile.setLastModificationTime(juce::Time::getCurrentTime());

        // If this version was previously skipped, don't show the alert again
        auto skippedVersion = lastVersionFile.loadFileAsString().trim();
        if (skippedVersion.isNotEmpty() && skippedVersion == latestRemoteVersion)
            return;

        auto isRemoteVersionNewer = isNewerVersionThanCurrent(remoteVersionString);

        if (isRemoteVersionNewer)
        {
            juce::AlertWindow::showYesNoCancelBox(
                juce::AlertWindow::InfoIcon,
                PLUGIN_DISPLAY_NAME,
                "A new version is available: " + remoteVersionString,
                "Download",
                "Skip this version",
                "Cancel",
                nullptr,
                this
            );
        }
    }

private:
    void modalStateFinished(int returnValue) override
    {
        if (returnValue == 1)
        {
            juce::URL("https://github.com/" + owner + "/" + repo + "/releases/latest/download/" + FILENAME)
                .launchInDefaultBrowser();
        }
        else if (returnValue == 2)
        {
            // User clicked "Skip this version"
            auto appDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                              .getChildFile(PLUGIN_DISPLAY_NAME);
            appDir.createDirectory();
            auto lastVersionFile = appDir.getChildFile("version_check");
            if (!latestRemoteVersion.isEmpty())
                lastVersionFile.replaceWithText(latestRemoteVersion);
        }
    }

    juce::String owner;
    juce::String repo;
    juce::String latestRemoteVersion;

    JUCE_DECLARE_SINGLETON(UpdateCheck, true)
};