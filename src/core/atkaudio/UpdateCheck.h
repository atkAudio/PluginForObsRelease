#pragma once
#include "LookAndFeel.h"
#include "atkaudio.h"
#include "../../CompareVersionStrings.h"

#include <atkaudio/Logging.h>
#include <config.h>
#include <juce_audio_utils/juce_audio_utils.h>

constexpr auto OWNER = "atkAudio";
constexpr auto DISPLAY_NAME = PLUGIN_DISPLAY_NAME;
constexpr auto REPO = "PluginForObsRelease";
constexpr auto VERSION = PLUGIN_VERSION;
constexpr auto JSON_VALUE = "tag_name";
constexpr auto FILENAME = "atkaudio-pluginforobs.zip";
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

        auto normalizedRemoteVersion = juce::String(NormalizeVersionString(remoteVersion.toStdString()));
        auto normalizedLocalVersion = juce::String(NormalizeVersionString(localVersion.toStdString()));

        if (normalizedRemoteVersion.isEmpty() || normalizedLocalVersion.isEmpty())
        {
            atk::logging::warning(
                "UpdateCheck::isNewerVersionThanCurrent",
                "unable to compare versions; remote=\"" + remoteVersion + "\" local=\"" + localVersion + "\""
            );
            return false;
        }

        auto comparison =
            CompareVersionStrings(normalizedRemoteVersion.toStdString(), normalizedLocalVersion.toStdString());

        atk::logging::debug(
            "UpdateCheck::isNewerVersionThanCurrent",
            "remote=\"" + remoteVersion + "\" local=\"" + localVersion + "\" comparison=" + juce::String(comparison)
        );

        return comparison > 0;
    }

private:
    void checkForUpdate()
    {
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

        // Past 7 days - do the check
        juce::URL versionURL("https://api.github.com/repos/" + owner + "/" + repo + "/releases/latest");

        int statusCode = 0;

        std::unique_ptr<juce::InputStream> inStream(versionURL.createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(5000)
                .withStatusCode(&statusCode)
        ));

        if (inStream == nullptr)
        {
            atk::logging::warning(
                "UpdateCheck::checkForUpdate",
                "request failed; statusCode=" + juce::String(statusCode)
            );
            return;
        }

        auto jsonResponse = inStream->readEntireStreamAsString().trim();

        if (jsonResponse.isEmpty())
        {
            atk::logging::warning("UpdateCheck::checkForUpdate", "empty response body from GitHub releases API");
            return;
        }

        auto remoteVersionString = getValueFromJson(jsonResponse, JSON_VALUE);
        releaseNotes = getValueFromJson(jsonResponse, "body");

        latestRemoteVersion = remoteVersionString;

        if (latestRemoteVersion.isEmpty())
        {
            atk::logging::warning(
                "UpdateCheck::checkForUpdate",
                "missing tag_name in response; payloadPrefix=\"" + jsonResponse.substring(0, 200) + "\""
            );
            return;
        }

        atk::logging::info(
            "UpdateCheck::checkForUpdate",
            "parsed versions; local=\""
                + juce::String(VERSION)
                + "\" remote=\""
                + latestRemoteVersion
                + "\" statusCode="
                + juce::String(statusCode)
        );

#ifdef SIMULATE_UPDATE_CHECK
        latestRemoteVersion = "99.99.99";
#endif
        if (ensureVersionFileExists(lastVersionFile))
        {
            // Update mod time now that we've checked
            lastVersionFile.setLastModificationTime(juce::Time::getCurrentTime());

#ifndef SIMULATE_UPDATE_CHECK
            // If this version was previously skipped, don't show the alert again
            auto skippedVersion = lastVersionFile.loadFileAsString().trim();
            if (skippedVersion.isNotEmpty() && skippedVersion == latestRemoteVersion)
            {
                atk::logging::info(
                    "UpdateCheck::checkForUpdate",
                    "suppressed alert because skippedVersion matches latestRemoteVersion; skippedVersion=\""
                        + skippedVersion
                        + "\""
                );
                return;
            }
#endif
        }

        auto isRemoteVersionNewer = isNewerVersionThanCurrent(latestRemoteVersion);

        atk::logging::info(
            "UpdateCheck::checkForUpdate",
            "comparison result; localVersion=\""
                + juce::String(VERSION)
                + "\" remoteVersion=\""
                + latestRemoteVersion
                + "\" isRemoteVersionNewer="
                + juce::String(isRemoteVersionNewer ? "true" : "false")
        );

        if (isRemoteVersionNewer)
        {
            auto message = "A new version is available: " + latestRemoteVersion;

            auto* updateAlert = new juce::AlertWindow(PLUGIN_DISPLAY_NAME, message, juce::AlertWindow::InfoIcon);
            updateAlert->addButton("Download", 1, juce::KeyPress(juce::KeyPress::returnKey));
            updateAlert->addButton("Skip this version", 2);
            updateAlert->addButton("Later", 0, juce::KeyPress(juce::KeyPress::escapeKey));

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
            // User clicked "Skip this version"
            auto lastVersionFile = atk::getSettingsFile("version");
            lastVersionFile.getParentDirectory().createDirectory();
            if (!latestRemoteVersion.isEmpty())
            {
                lastVersionFile.replaceWithText(latestRemoteVersion);
                atk::logging::info(
                    "UpdateCheck::modalStateFinished",
                    "user skipped remoteVersion=\""
                        + latestRemoteVersion
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
    juce::String releaseNotes;
    bool hasCheckedForUpdate = false;

    JUCE_DECLARE_SINGLETON(UpdateCheck, true)
};
