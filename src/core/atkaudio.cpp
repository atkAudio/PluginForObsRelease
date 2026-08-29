#include "core/atkaudio/atkaudio.h"

#include <atkaudio/AudioProcessorGraphMT/RealtimeThreadPool.h>
#include <atkaudio/Logging.h>
#include <atkaudio/LookAndFeel.h>
#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <atkaudio/ObsJucePluginFormatLifecycle.h>
#include <atkaudio/UpdateCheck.h>

#include <juce_audio_utils/juce_audio_utils.h>

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <obs-module.h>
#include <obs-frontend-api.h>

UpdateCheck* updateCheck = nullptr;

// Store Qt main window handle for per-instance parent creation (lazy-initialized)
static void* g_qtMainWindowHandle = nullptr;
static bool g_qtMainWindowInitialized = false;

namespace
{
void applyThemeColorsFromObsMainWindow()
{
    auto* mainQWidget = static_cast<QWidget*>(obs_frontend_get_main_window());
    if (mainQWidget == nullptr)
    {
        atk::logging::warning("UI", "applyThemeColorsFromObsMainWindow: obs_frontend_get_main_window returned null");
        return;
    }

    QPalette palette = mainQWidget->palette();
    QColor bgColor = palette.color(QPalette::Window);
    QColor fgColor = palette.color(QPalette::WindowText);

    auto bgColour = juce::Colour(bgColor.red(), bgColor.green(), bgColor.blue());
    auto fgColour = juce::Colour(fgColor.red(), fgColor.green(), fgColor.blue());
    atk::LookAndFeel::applyColorsToInstance(bgColour, fgColour);

    atk::logging::debug("UI", "applyThemeColorsFromObsMainWindow: applied OBS theme colors");
}
} // namespace

juce::File atk::getSettingsFile(const juce::String& name)
{
    auto* module = obs_current_module();
    if (module == nullptr)
    {
        atk::logging::error("SETTINGS", "Settings path resolution requires an OBS module context");
        return {};
    }

    auto filename = name + ".settings";
    char* obsPath = obs_module_get_config_path(module, filename.toRawUTF8());
    if (obsPath == nullptr)
    {
        const char* binaryPath = obs_get_module_binary_path(module);
        atk::logging::error(
            "SETTINGS",
            "obs_module_get_config_path returned nullptr for settings file: "
                + name
                + ", moduleBinaryPath="
                + juce::String(binaryPath ? binaryPath : "(unknown)")
        );
        return {};
    }

    juce::File result(obsPath);
    bfree(obsPath);

    auto parentDir = result.getParentDirectory();
    if (!parentDir.exists() && !parentDir.createDirectory())
    {
        atk::logging::error(
            "SETTINGS",
            "failed to create settings directory for \"" + name + "\"; path=\"" + parentDir.getFullPathName() + "\""
        );
    }

    // No info-level log here: this runs while GlobalSettings' bootstrap holds g_settingsMutex,
    // and info logging calls isLoggingEnabled(), which re-locks that same non-recursive mutex.
    return result;
}

bool atk::create()
{
    atk::logging::info("LIFECYCLE", "atk::create begin");

    auto& lifecycle = atk::ObsJucePluginFormatLifecycle::getInstance();
    if (!lifecycle.initialize())
    {
        atk::logging::error("LIFECYCLE", "atk::create failed to initialize OBS JUCE lifecycle");
        return false;
    }

    // Initialize LookAndFeel singleton
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    // Initialize MIDI server
    if (auto* midiServer = atk::MidiServer::getInstance())
        midiServer->initialize();

    // Initialize Audio server
    if (auto* audioServer = atk::AudioServer::getInstance())
        audioServer->initialize();

    // Initialize RealtimeThreadPool synchronously so it's ready when filters are created
    if (auto* threadPool = atk::RealtimeThreadPool::getInstance())
        threadPool->initialize();

    atk::logging::info("LIFECYCLE", "atk::create completed");

    return true;
}

void atk::pump()
{
    atk::ObsJucePluginFormatLifecycle::getInstance().pumpPendingMessages();
}

bool atk::startMessagePump(QObject* qtParent)
{
    auto& lifecycle = atk::ObsJucePluginFormatLifecycle::getInstance();
    if (!lifecycle.startMessagePump(qtParent))
    {
        atk::logging::error("LIFECYCLE", "atk::startMessagePump failed");
        return false;
    }

    atk::logging::info("LIFECYCLE", "atk::startMessagePump completed");

    return true;
}

bool atk::isReady()
{
    return atk::ObsJucePluginFormatLifecycle::getInstance().isReady();
}

bool atk::isShuttingDown()
{
    return atk::ObsJucePluginFormatLifecycle::getInstance().isShuttingDown();
}

void atk::destroy()
{
    atk::logging::info("LIFECYCLE", "atk::destroy begin");

    auto& lifecycle = atk::ObsJucePluginFormatLifecycle::getInstance();

    if (auto* midiServer = atk::MidiServer::getInstance())
    {
        midiServer->shutdown();
        atk::MidiServer::deleteInstance();
    }

    if (auto* audioServer = atk::AudioServer::getInstance())
    {
        audioServer->shutdown();
        atk::AudioServer::deleteInstance();
    }

    if (auto* threadPool = atk::RealtimeThreadPool::getInstance())
    {
        threadPool->shutdown();
        atk::RealtimeThreadPool::deleteInstance();
    }

    lifecycle.shutdown();

    atk::logging::info("LIFECYCLE", "atk::destroy completed");
}

void atk::update()
{
    if (updateCheck == nullptr)
        updateCheck = new UpdateCheck(); // deleted at shutdown

    updateCheck->checkForUpdateOnStartupOnce();
}

void* atk::getQtMainWindowHandle()
{
    // Fully lazy initialization: get Qt window, extract handle, and apply colors on first access
    if (!g_qtMainWindowInitialized)
    {
        // Get Qt main window from OBS frontend API
        QWidget* mainQWidget = (QWidget*)obs_frontend_get_main_window();
        if (!mainQWidget)
        {
            atk::logging::warning("UI", "getQtMainWindowHandle: obs_frontend_get_main_window returned null");
            return nullptr;
        }

        // Extract native window handle
        void* nativeHandle = nullptr;
#ifdef _WIN32
        nativeHandle = reinterpret_cast<void*>(mainQWidget->winId());
#elif defined(__APPLE__)
        if (auto* window = mainQWidget->windowHandle())
            nativeHandle = reinterpret_cast<void*>(window->winId());
#elif defined(__linux__)
        nativeHandle = reinterpret_cast<void*>(mainQWidget->winId());
#endif

        if (nativeHandle)
        {
            g_qtMainWindowHandle = nativeHandle;
            atk::logging::debug("UI", "getQtMainWindowHandle: extracted native handle");
        }
        else
        {
            atk::logging::warning("UI", "getQtMainWindowHandle: failed to extract native handle");
        }

        g_qtMainWindowInitialized = true;
    }

    // Re-apply colors whenever this is called.
    applyThemeColorsFromObsMainWindow();

    // Return the cached Qt main window handle
    return g_qtMainWindowHandle;
}

void atk::setWindowOwnership(juce::Component* component)
{
    // With the invisible parent component attached to Qt, JUCE automatically
    // handles the window hierarchy. No manual platform-specific code needed!
    // All JUCE windows are now children of our parent component.
    (void)component; // Unused - kept for API compatibility
}

void atk::applyColors(uint8_t bgR, uint8_t bgG, uint8_t bgB, uint8_t fgR, uint8_t fgG, uint8_t fgB)
{
    auto bgColour = juce::Colour(bgR, bgG, bgB);
    auto fgColour = juce::Colour(fgR, fgG, fgB);
    atk::LookAndFeel::applyColorsToInstance(bgColour, fgColour);
}

void atk::logMessage(const juce::String& message)
{
    atk::logging::info("ATK", message);
}
