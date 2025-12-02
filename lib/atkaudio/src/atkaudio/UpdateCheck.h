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

// Define to always check for updates and simulate newer version available
// #define SIMULATE_UPDATE_CHECK

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

#ifndef SIMULATE_UPDATE_CHECK
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
            DBG("last modification time: " << lastVersionFile.getLastModificationTime().toString(true, true));
            return;
        }
#endif

        // Past 7 days - do the check
        juce::URL versionURL("https://api.github.com/repos/" + owner + "/" + repo + "/releases/latest");

        std::unique_ptr<juce::InputStream> inStream(versionURL.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs(5000)
        ));

        if (inStream == nullptr)
            return;

        auto jsonResponse = inStream->readEntireStreamAsString().trim();

        if (jsonResponse.isEmpty())
            return;

        auto remoteVersionString = getValueFromJson(jsonResponse, JSON_VALUE);
        releaseNotes = getValueFromJson(jsonResponse, "body");

        latestRemoteVersion = remoteVersionString;

#ifdef SIMULATE_UPDATE_CHECK
        latestRemoteVersion = "99.99.99";
#endif

#ifndef SIMULATE_UPDATE_CHECK
        // Update mod time now that we've checked
        lastVersionFile.setLastModificationTime(juce::Time::getCurrentTime());

        // If this version was previously skipped, don't show the alert again
        auto skippedVersion = lastVersionFile.loadFileAsString().trim();
        if (skippedVersion.isNotEmpty() && skippedVersion == latestRemoteVersion)
            return;
#endif

        auto isRemoteVersionNewer = isNewerVersionThanCurrent(latestRemoteVersion);

        if (isRemoteVersionNewer)
        {
            juce::String message = "A new version is available: " + latestRemoteVersion;
            if (releaseNotes.isNotEmpty())
            {
                auto formattedNotes = releaseNotes.replace("\n", "\n\n");
                message += "\n\n" + formattedNotes;
            }

            juce::AlertWindow::showYesNoCancelBox(
                juce::AlertWindow::InfoIcon,
                PLUGIN_DISPLAY_NAME,
                message,
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
    juce::String releaseNotes;

    JUCE_DECLARE_SINGLETON(UpdateCheck, true)
};