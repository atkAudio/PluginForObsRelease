#include "atkaudio.h"

#include "AudioProcessorGraphMT/AudioThreadPool.h"
#include "JuceApp.h"
#include "LookAndFeel.h"
#include "MessagePump.h"
#include "ModuleInfrastructure/AudioServer/AudioServer.h"
#include "ModuleInfrastructure/MidiServer/MidiServer.h"
#include "UpdateCheck.h"

#include <juce_audio_utils/juce_audio_utils.h>

#ifdef ENABLE_QT
#include <QColor>
#include <QPalette>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#endif

#include <obs-frontend-api.h>

UpdateCheck* updateCheck = nullptr;
atk::MessagePump* g_messagePump = nullptr;

// Store Qt main window handle for per-instance parent creation (lazy-initialized)
static void* g_qtMainWindowHandle = nullptr;
static bool g_qtMainWindowInitialized = false;

void atk::create()
{
// Set createInstance to make JUCE think this is a standalone app.
// This enables per-monitor DPI awareness on Windows via setDPIAwareness().
#ifndef JUCE_MAC
    juce::JUCEApplicationBase::createInstance = []() -> juce::JUCEApplicationBase* { return nullptr; };
#endif
    juce::initialiseJuce_GUI();

    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();

    // Initialize LookAndFeel singleton
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;

    // Sync OBS theme colors to JUCE LookAndFeel
    getQtMainWindowHandle();

    // Initialize MIDI server
    if (auto* midiServer = atk::MidiServer::getInstance())
        midiServer->initialize();

    // Initialize Audio server
    if (auto* audioServer = atk::AudioServer::getInstance())
        audioServer->initialize();

    // Initialize AudioThreadPool synchronously so it's ready when filters are created
    if (auto* threadPool = atk::AudioThreadPool::getInstance())
        threadPool->initialize();
}

void atk::pump()
{
#if JUCE_LINUX
    // On Linux, we need to pump the JUCE message loop
    // Use dispatchPendingMessages() instead of runDispatchLoopUntil() to avoid
    // conflicts with Qt's event loop - we just want to process pending JUCE messages
    // without polling/blocking for new ones
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
    {
        // Only dispatch if we're on the message thread
        if (mm->isThisTheMessageThread())
        {
            // Process any pending async callbacks
            mm->runDispatchLoopUntil(0); // 0ms = just process pending, don't wait
        }
    }
#endif
}

void atk::startMessagePump(QObject* qtParent)
{
#if JUCE_LINUX
    // On Linux, we need a Qt timer-based message pump for JUCE
    if (g_messagePump)
    {
        DBG("startMessagePump: MessagePump already started");
        return;
    }

    g_messagePump = new atk::MessagePump(qtParent);
#else
    // On macOS and Windows, we don't need a message pump - JUCE integrates with native event loops
    (void)qtParent;
#endif
}

void atk::destroy()
{
    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();

    g_messagePump = nullptr;

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

    if (auto* threadPool = atk::AudioThreadPool::getInstance())
    {
        threadPool->shutdown();
        atk::AudioThreadPool::deleteInstance();
    }

    juce::shutdownJuce_GUI();
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
            DBG("getQtMainWindowHandle: obs_frontend_get_main_window() returned null");
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
            DBG("getQtMainWindowHandle: Extracted native handle on first access");
            DBG("  Native handle: " + juce::String::toHexString((juce::pointer_sized_int)nativeHandle));
        }
        else
        {
            DBG("getQtMainWindowHandle: Failed to extract native handle");
        }

        // Apply OBS theme colors to JUCE
        QPalette palette = mainQWidget->palette();
        QColor bgColor = palette.color(QPalette::Window);
        QColor fgColor = palette.color(QPalette::WindowText);

        auto bgColour = juce::Colour(bgColor.red(), bgColor.green(), bgColor.blue());
        auto fgColour = juce::Colour(fgColor.red(), fgColor.green(), fgColor.blue());
        atk::LookAndFeel::applyColorsToInstance(bgColour, fgColour);

        DBG("getQtMainWindowHandle: Applied OBS theme colors");
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
    DBG(message);
}
