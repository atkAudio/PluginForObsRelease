#pragma once
#include "LookAndFeel.h"
#include "atkaudio.h"
#include "update_manifest.h"

#include <atkaudio/Logging.h>
#include <config.h>
#include <juce_audio_utils/juce_audio_utils.h>

constexpr auto OWNER = UPDATE_REPOSITORY_OWNER;
constexpr auto DISPLAY_NAME = PLUGIN_DISPLAY_NAME;
constexpr auto REPO = UPDATE_REPOSITORY_NAME;
constexpr auto VERSION = PLUGIN_VERSION;
constexpr auto FILENAME = UPDATE_FILENAME;
constexpr long long VERSION_FILE_EXPIRY_MS = 3LL * 30 * 24 * 60 * 60 * 1000;
constexpr long long UPDATE_CHECK_INTERVAL_MS = 7LL * 24 * 60 * 60 * 1000;
constexpr int RELEASE_NOTES_WIDTH_PX = 640;
constexpr int RELEASE_NOTES_HEIGHT_PX = 400;
constexpr int UPDATE_PROMPT_BUTTON_TOP_MARGIN_PX = 10;

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
    }

    void checkForUpdateOnStartupOnce()
    {
        if (hasCheckedForUpdate)
            return;

        hasCheckedForUpdate = true;
        checkForUpdate();
    }

    static juce::String getValueFromJson(const juce::String& jsonString, const juce::String& key)
    {
        juce::var json = juce::JSON::parse(jsonString);
        if (json.isObject())
        {
            auto jsonObject = json.getDynamicObject();
            if (jsonObject->hasProperty(key))
            {
                auto value = jsonObject->getProperty(key);
                if (value.isString())
                    return value.toString();
            }
        }
        return {};
    }

private:
    void checkForUpdate()
    {
        if (!atk::updates::IsValidBuildTimestamp(PLUGIN_BUILD_TIMESTAMP))
        {
            atk::logging::warning("UpdateCheck::checkForUpdate", "invalid compiled build timestamp");
            return;
        }

        juce::File lastVersionFile = atk::getSettingsFile("version");
        if (lastVersionFile.existsAsFile())
        {
            auto now = juce::Time::getCurrentTime();
            if (now.toMilliseconds() - lastVersionFile.getCreationTime().toMilliseconds() > VERSION_FILE_EXPIRY_MS)
                lastVersionFile.deleteFile();
        }

#ifndef SIMULATE_UPDATE_CHECK
        if (lastVersionFile.existsAsFile())
        {
            auto now = juce::Time::getCurrentTime();
            if (now.toMilliseconds() - lastVersionFile.getLastModificationTime().toMilliseconds()
                < UPDATE_CHECK_INTERVAL_MS)
                return;
        }
#endif

        atk::updates::Manifest remoteManifest;
#ifndef SIMULATE_UPDATE_CHECK
        juce::URL manifestUrl(UPDATE_MANIFEST_URL);
        int statusCode = 0;
        std::unique_ptr<juce::InputStream> inStream(manifestUrl.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(5000)
                .withNumRedirectsToFollow(5)
                .withStatusCode(&statusCode)
        ));

        if (inStream == nullptr || statusCode != 200)
        {
            atk::logging::warning(
                "UpdateCheck::checkForUpdate",
                "manifest request failed; statusCode=" + juce::String(statusCode)
            );
            return;
        }

        auto result = atk::updates::ParseManifest(inStream->readEntireStreamAsString(), PLUGIN_NAME, &remoteManifest);
        if (result.failed())
        {
            atk::logging::warning("UpdateCheck::checkForUpdate", result.getErrorMessage());
            return;
        }
#else
        remoteManifest = {PLUGIN_NAME, "99.99.99", "99991231-235959", UPDATE_MANIFEST_URL};
#endif
        latestRemoteVersion = remoteManifest.version;
        latestRemoteBuildTimestamp = remoteManifest.buildTimestamp;

        atk::logging::info(
            "UpdateCheck::checkForUpdate",
            "parsed builds; local=\""
                + juce::String(PLUGIN_BUILD_TIMESTAMP)
                + "\" remote=\""
                + latestRemoteBuildTimestamp
                + "\" localVersion=\""
                + juce::String(VERSION)
                + "\" remoteVersion=\""
                + latestRemoteVersion
                + "\""
        );

        juce::String skippedTimestamp;
        if (ensureVersionFileExists(lastVersionFile))
        {
            lastVersionFile.setLastModificationTime(juce::Time::getCurrentTime());
#ifndef SIMULATE_UPDATE_CHECK
            skippedTimestamp = lastVersionFile.loadFileAsString().trim();
#endif
        }

        if (atk::updates::ShouldOfferUpdate(&remoteManifest, PLUGIN_BUILD_TIMESTAMP, skippedTimestamp))
        {
            fetchReleaseNotes();
            auto message =
                "A new build is available: " + latestRemoteVersion + "\nBuild: " + latestRemoteBuildTimestamp + " UTC";

            auto* updateAlert = new juce::AlertWindow(PLUGIN_DISPLAY_NAME, message, juce::AlertWindow::InfoIcon);
            updateAlert->addButton("Download", 1, juce::KeyPress(juce::KeyPress::returnKey));
            updateAlert->addButton("Skip this build", 2);
            updateAlert->addButton("Later", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            if (releaseNotes.isNotEmpty())
            {
                auto* releaseNotesEditor = new juce::TextEditor();
                releaseNotesEditor->setMultiLine(true, true);
                releaseNotesEditor->setReadOnly(true);
                releaseNotesEditor->setScrollbarsShown(true);
                releaseNotesEditor->setCaretVisible(false);
                releaseNotesEditor->setPopupMenuEnabled(true);
                releaseNotesEditor->setOpaque(false);
                releaseNotesEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
                releaseNotesEditor->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
                releaseNotesEditor->setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
                releaseNotesEditor->setColour(
                    juce::TextEditor::textColourId,
                    updateAlert->findColour(juce::AlertWindow::backgroundColourId).contrasting()
                );
                releaseNotesEditor->setText(releaseNotes, false);

                releaseNotesEditor->setSize(RELEASE_NOTES_WIDTH_PX, RELEASE_NOTES_HEIGHT_PX);

                updateAlert->addCustomComponent(releaseNotesEditor);
            }
            updateAlert->setAlwaysOnTop(true);
            updateAlert->enterModalState(true, this, true);
            updateAlert->toFront(true);

            juce::Component::SafePointer<juce::AlertWindow> safeAlert(updateAlert);
            juce::MessageManager::callAsync(
                [safeAlert]()
                {
                    if (safeAlert == nullptr)
                        return;

                    safeAlert->setAlwaysOnTop(true);
                    safeAlert->toFront(true);

                    for (int buttonIndex = 0; buttonIndex < safeAlert->getNumButtons(); ++buttonIndex)
                    {
                        auto* button = safeAlert->getButton(buttonIndex);
                        if (button != nullptr)
                            button->setTopLeftPosition(
                                button->getX(),
                                button->getY() + UPDATE_PROMPT_BUTTON_TOP_MARGIN_PX
                            );
                    }
                }
            );

            atk::logging::info(
                "UpdateCheck::checkForUpdate",
                "displayed update prompt for remoteVersion=\"" + latestRemoteVersion + "\""
            );
        }
    }

private:
    void fetchReleaseNotes()
    {
        releaseNotes.clear();
        juce::URL notesUrl("https://api.github.com/repos/" + owner + "/" + repo + "/releases/latest");
        int statusCode = 0;
        std::unique_ptr<juce::InputStream> stream(notesUrl.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(5000)
                .withNumRedirectsToFollow(5)
                .withStatusCode(&statusCode)
        ));
        if (stream == nullptr || statusCode != 200)
        {
            atk::logging::warning(
                "UpdateCheck::fetchReleaseNotes",
                "release notes unavailable; showing update without notes"
            );
            return;
        }
        auto json = stream->readEntireStreamAsString();
        auto tag = getValueFromJson(json, "tag_name");
        if (tag == latestRemoteVersion || tag == "v" + latestRemoteVersion)
            releaseNotes = getValueFromJson(json, "body");
    }

    bool ensureVersionFileExists(juce::File& versionFile)
    {
        if (versionFile.getFullPathName().isEmpty())
        {
            atk::logging::warning(
                "UpdateCheck::ensureVersionFileExists",
                "settings file path is empty; unable to persist version state"
            );
            return false;
        }

        auto versionDirectory = versionFile.getParentDirectory();
        if (!versionDirectory.exists() && !versionDirectory.createDirectory())
        {
            atk::logging::warning(
                "UpdateCheck::ensureVersionFileExists",
                "failed to create settings directory; path=\"" + versionDirectory.getFullPathName() + "\""
            );
            return false;
        }

        if (!versionFile.existsAsFile() && !versionFile.replaceWithText(""))
        {
            atk::logging::warning(
                "UpdateCheck::ensureVersionFileExists",
                "failed to create version file; path=\"" + versionFile.getFullPathName() + "\""
            );
            return false;
        }

        return true;
    }

    void modalStateFinished(int returnValue) override
    {
        if (returnValue == 1)
        {
            atk::logging::info(
                "UpdateCheck::modalStateFinished",
                "user selected Download for remoteVersion=\"" + latestRemoteVersion + "\""
            );
            juce::URL("https://github.com/" + owner + "/" + repo + "/releases/latest/download/" + FILENAME)
                .launchInDefaultBrowser();
        }
        else if (returnValue == 2)
        {
            auto lastVersionFile = atk::getSettingsFile("version");
            if (ensureVersionFileExists(lastVersionFile) && !latestRemoteBuildTimestamp.isEmpty())
            {
                if (!lastVersionFile.replaceWithText(latestRemoteBuildTimestamp))
                {
                    atk::logging::warning("UpdateCheck::modalStateFinished", "unable to save skipped build timestamp");
                    return;
                }
                atk::logging::info(
                    "UpdateCheck::modalStateFinished",
                    "user skipped remoteBuildTimestamp=\""
                        + latestRemoteBuildTimestamp
                        + "\" savedTo=\""
                        + lastVersionFile.getFullPathName()
                        + "\""
                );
            }
        }
        else
        {
            atk::logging::info(
                "UpdateCheck::modalStateFinished",
                "user chose Later on update prompt for remoteVersion=\"" + latestRemoteVersion + "\""
            );
        }
    }

    juce::String owner;
    juce::String repo;
    juce::String latestRemoteVersion;
    juce::String latestRemoteBuildTimestamp;
    juce::String releaseNotes;
    bool hasCheckedForUpdate = false;

    JUCE_DECLARE_SINGLETON(UpdateCheck, true)
};
