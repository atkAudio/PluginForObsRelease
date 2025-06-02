#include "MessagePump.h"

#include <atkaudio/atkaudio.h>
#include <juce_gui_basics/juce_gui_basics.h>

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
