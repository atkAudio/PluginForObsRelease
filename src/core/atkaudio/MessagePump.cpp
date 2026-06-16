#include "MessagePump.h"
#include "Logging.h"
#include "atkaudio.h"

#include <juce_events/juce_events.h>

#include <QTimer>

namespace atk
{

MessagePump::MessagePump(QObject* parent)
    : QObject(nullptr)
    , timer(this)
{
    atk::logging::debug("MessagePump::ctor", "MessagePump constructor called");

    (void)parent;

    // Verify JUCE MessageManager is attached to the current (Qt main) thread
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
        atk::logging::warning("MessagePump::ctor", "JUCE MessageManager is not attached to the Qt main thread");

    // On macOS, JUCE registers a CFRunLoopSource on the main CFRunLoop
    // (kCFRunLoopCommonModes). Qt drives the main CFRunLoop as part of its own
    // event loop, so JUCE messages are delivered implicitly — no explicit pump
    // call is needed.
    //
    // On Windows, JUCE creates a hidden HWND and posts messages to it via
    // PostMessage/SendNotifyMessage. Qt runs a Win32 message loop on the main
    // thread which dispatches to all HWNDs, including JUCE's hidden one —
    // again fully implicit.
    //
    // On Linux there is no shared OS message loop between Qt and JUCE, so we
    // must call atk::pump() explicitly on each tick (see onTimeout).
#ifdef JUCE_LINUX
    connect(&timer, &QTimer::timeout, this, &MessagePump::onTimeout);
    timer.start(10);
#endif
}

MessagePump::~MessagePump()
{
    atk::logging::debug("MessagePump::dtor", "MessagePump destructor called");
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
