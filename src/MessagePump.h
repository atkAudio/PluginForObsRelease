#pragma once
#include <atkaudio/atkaudio.h>

#if defined(JUCE_WINDOWS) && defined(JUCE_DEBUG)
#define NO_MESSAGE_PUMP
#endif
#ifndef NO_MESSAGE_PUMP

#include <QTimer>

// parent handles lifetime

class MessagePump : public QObject
{
    Q_OBJECT

public:
    MessagePump(QObject* parent = nullptr);
    ~MessagePump();

private slots:

    void onTimeout();

private:
    QTimer* timer;
};
#endif