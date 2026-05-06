#pragma once

#include <QObject>
#include <QTimer>
#include <atomic>

namespace atk
{

// MessagePump bridges Qt event loop with JUCE MessageManager
// Lifetime is owned by ObsJucePluginFormatLifecycle (std::unique_ptr).
// The ctor keeps QObject* for API compatibility, but does not transfer Qt ownership.
class MessagePump : public QObject
{
    Q_OBJECT

public:
    explicit MessagePump(QObject* parent = nullptr);
    ~MessagePump();

    void stopPump();

private slots:
    void onTimeout();

private:
    std::atomic_bool needsToStop{false};
    QTimer timer;
};

} // namespace atk
