#include "MessagePump.h"

#include <juce_events/juce_events.h>
#include <obs-module.h>

MessagePump::MessagePump(QObject* parent)
    : QObject(parent)
{
    // Verify JUCE MessageManager is attached to the current (Qt main) thread
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
        blog(LOG_ERROR, "MessagePump: JUCE MessageManager is NOT attached to Qt main thread!");
    else
        blog(LOG_INFO, "MessagePump: JUCE MessageManager correctly attached to Qt main thread");

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MessagePump::onTimeout);
    timer->start(10);
}

MessagePump::~MessagePump()
{
}

void MessagePump::stopPump()
{
    needsToStop.store(true, std::memory_order_release);
}

void MessagePump::onTimeout()
{
    if (needsToStop.load(std::memory_order_acquire))
        return;

    atk::pump();
}