#pragma once
#include <QTimer>
#include <atkaudio/atkaudio.h>

// parent handles lifetime

class MessagePump : public QObject
{
    Q_OBJECT

public:
    MessagePump(QObject* parent = nullptr);
    ~MessagePump();

    void stopPump();

private slots:

    void onTimeout();

private:
    std::atomic_bool needsToStop{false};
    QTimer* timer;
};
