#include "ObsJucePluginFormatLifecycle.h"

#include <atkaudio/MessagePump.h>

#include <juce_events/juce_events.h>

namespace atk
{

ObsJucePluginFormatLifecycle::ObsJucePluginFormatLifecycle() = default;
ObsJucePluginFormatLifecycle::~ObsJucePluginFormatLifecycle() = default;

ObsJucePluginFormatLifecycle& ObsJucePluginFormatLifecycle::getInstance()
{
    static ObsJucePluginFormatLifecycle instance;
    return instance;
}

bool ObsJucePluginFormatLifecycle::initialize()
{
    const auto currentPhase = lifecyclePhase.load(std::memory_order_acquire);

    if (currentPhase == ObsJucePluginFormatLifecyclePhase::Ready)
        return true;

    if (currentPhase == ObsJucePluginFormatLifecyclePhase::Initializing
        || currentPhase == ObsJucePluginFormatLifecyclePhase::ShuttingDown)
        return false;

    lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::Initializing, std::memory_order_release);

    // Make JUCE treat this host integration as app-context for Windows platform
    // init hooks (notably per-monitor DPI awareness setup in windowing code).
#if JUCE_WINDOWS
    juce::JUCEApplicationBase::createInstance = []() -> juce::JUCEApplicationBase* { return nullptr; };
#endif

    juceRuntime = std::make_unique<juce::ScopedJuceInitialiser_GUI>();

    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (messageManager == nullptr)
    {
        juceRuntime.reset();
        lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::Uninitialized, std::memory_order_release);
        DBG("ObsJucePluginFormatLifecycle::initialize: failed to create MessageManager");
        return false;
    }

    messageManager->setCurrentThreadAsMessageThread();

    lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::Ready, std::memory_order_release);
    return true;
}

bool ObsJucePluginFormatLifecycle::startMessagePump(QObject* qtParent)
{
    if (!initialize())
        return false;

    if (messagePump != nullptr)
        return true;

    messagePump = std::make_unique<MessagePump>(qtParent);
    return true;
}

void ObsJucePluginFormatLifecycle::pumpPendingMessages()
{
    if (!isReady())
        return;

    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (messageManager->isThisTheMessageThread())
            messageManager->runDispatchLoopUntil(0);
    }
}

void ObsJucePluginFormatLifecycle::shutdown()
{
    const auto currentPhase = lifecyclePhase.load(std::memory_order_acquire);

    if (currentPhase == ObsJucePluginFormatLifecyclePhase::Uninitialized
        || currentPhase == ObsJucePluginFormatLifecyclePhase::Shutdown)
        return;

    lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::ShuttingDown, std::memory_order_release);

    if (messagePump != nullptr)
    {
        messagePump->stopPump();
        messagePump.reset();
    }

    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (messageManager->isThisTheMessageThread())
            messageManager->runDispatchLoopUntil(0);
    }

    juceRuntime.reset();

    lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::Shutdown, std::memory_order_release);
}

ObsJucePluginFormatLifecyclePhase ObsJucePluginFormatLifecycle::phase() const
{
    return lifecyclePhase.load(std::memory_order_acquire);
}

bool ObsJucePluginFormatLifecycle::isReady() const
{
    return phase() == ObsJucePluginFormatLifecyclePhase::Ready;
}

bool ObsJucePluginFormatLifecycle::isShuttingDown() const
{
    return phase() == ObsJucePluginFormatLifecyclePhase::ShuttingDown;
}

} // namespace atk
