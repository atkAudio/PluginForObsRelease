#include "MessagePump.h"
#include "atkaudio.h"

#include <juce_events/juce_events.h>

#include <QTimer>

namespace atk
{

MessagePump::MessagePump(QObject* parent)
    : QObject(nullptr)
    , timer(this)
{
    (void)parent;

    // Verify JUCE MessageManager is attached to the current (Qt main) thread
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        // Log error - but don't use blog() here since we're in atkaudio lib
        juce::Logger::writeToLog("MessagePump: ERROR - JUCE MessageManager is NOT attached to Qt main thread!");
    }

    connect(&timer, &QTimer::timeout, this, &MessagePump::onTimeout);
    timer.start(10);
}

MessagePump::~MessagePump()
{
    stopPump();
}

void MessagePump::stopPump()
{
    // Prevent future callbacks and synchronously stop the Qt timer while the
    // message loop is still alive.
    needsToStop.store(true, std::memory_order_release);

    if (timer.isActive())
        timer.stop();
}

void MessagePump::onTimeout()
{
    if (needsToStop.load(std::memory_order_acquire))
        return;

    atk::pump();
}

} // namespace atk
