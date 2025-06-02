#pragma once

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