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

void MessagePump::onTimeout()
{
    atk::pump();
}
#endif