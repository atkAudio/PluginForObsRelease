#include "MessagePump.h"
#ifndef NO_MESSAGE_PUMP

MessagePump::MessagePump(QObject* parent)
    : QObject(parent)
{
    atk::create();
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MessagePump::onTimeout);
    timer->start(10);
}

MessagePump::~MessagePump()
{
    atk::destroy();
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
#endif