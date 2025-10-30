#include "MessagePump.h"

#include "atkaudio.h"

#include <QTimer>
#include <juce_events/juce_events.h>

namespace atk
{

MessagePump::MessagePump(QObject* parent)
    : QObject(parent)
{
    // Verify JUCE MessageManager is attached to the current (Qt main) thread
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        // Log error - but don't use blog() here since we're in atkaudio lib
        juce::Logger::writeToLog("MessagePump: ERROR - JUCE MessageManager is NOT attached to Qt main thread!");
    }

    // Create timer without parent so we control its lifetime
    // This prevents crash if Qt parent is destroyed before we are
    timer = new QTimer(nullptr);
    connect(timer, &QTimer::timeout, this, &MessagePump::onTimeout);
    timer->start(10);
}

MessagePump::~MessagePump()
{
    // Don't touch timer in destructor - Qt may already be shutting down
    // The needsToStop flag will prevent further callbacks
    timer = nullptr;
}

void MessagePump::stopPump()
{
    // Just set the flag - don't touch Qt objects during shutdown
    // The timer callback will see this and stop pumping
    needsToStop.store(true, std::memory_order_release);
}

void MessagePump::onTimeout()
{
    if (needsToStop.load(std::memory_order_acquire))
        return;

    atk::pump();
}

} // namespace atk
