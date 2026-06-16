#include "core/atkaudio/atkaudio.h"

#include <atkaudio/AudioProcessorGraphMT/RealtimeThreadPool.h>
#include <atkaudio/Logging.h>
#include <atkaudio/LookAndFeel.h>
#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <atkaudio/ObsJucePluginFormatLifecycle.h>
#include <atkaudio/UpdateCheck.h>

#include <juce_audio_utils/juce_audio_utils.h>

#ifdef ENABLE_QT
#include <QColor>
#include <QPalette>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>

UpdateCheck* updateCheck = nullptr;

// Store Qt main window handle for per-instance parent creation (lazy-initialized)
static void* g_qtMainWindowHandle = nullptr;
static bool g_qtMainWindowInitialized = false;

static juce::File getFallbackSettingsFile(const juce::String& name)
{
    juce::PropertiesFile::Options opts;
    opts.applicationName = name;
    opts.filenameSuffix = "settings";
    opts.osxLibrarySubFolder = "Application Support";
    opts.folderName = "atkAudio Plugin";
    return opts.getDefaultFile();
}

juce::File atk::getSettingsFile(const juce::String& name)
{
    auto* module = obs_current_module();
    if (module == nullptr)
        return getFallbackSettingsFile(name);

    auto filename = name + ".settings";
    char* obsPath = obs_module_get_config_path(module, filename.toRawUTF8());
    if (obsPath == nullptr)
        return getFallbackSettingsFile(name);

    juce::File result(obsPath);
    bfree(obsPath);
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

    // Sync OBS theme colors to JUCE LookAndFeel.
    // Bypass only true Debug builds (_DEBUG without NDEBUG), but keep this enabled
    // in RelWithDebInfo where _DEBUG may still be defined by toolchain settings.
#if !(defined(_DEBUG) && !defined(NDEBUG))
    getQtMainWindowHandle();
#endif

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
}

void* atk::getQtMainWindowHandle()
{
    // Fully lazy initialization: get Qt window, extract handle, and apply colors on first access
    if (!g_qtMainWindowInitialized)
    {
        g_qtMainWindowInitialized = true;

#ifdef ENABLE_QT
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

        // Apply OBS theme colors to JUCE
        QPalette palette = mainQWidget->palette();
        QColor bgColor = palette.color(QPalette::Window);
        QColor fgColor = palette.color(QPalette::WindowText);

        auto bgColour = juce::Colour(bgColor.red(), bgColor.green(), bgColor.blue());
        auto fgColour = juce::Colour(fgColor.red(), fgColor.green(), fgColor.blue());
        atk::LookAndFeel::applyColorsToInstance(bgColour, fgColour);

        atk::logging::debug("UI", "getQtMainWindowHandle: applied OBS theme colors");
#endif
    }

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
