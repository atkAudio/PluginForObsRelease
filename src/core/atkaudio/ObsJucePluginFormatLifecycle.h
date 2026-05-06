#pragma once

#include <atomic>
#include <memory>

class QObject;

namespace juce
{
class ScopedJuceInitialiser_GUI;
}

namespace atk
{

class MessagePump;

enum class ObsJucePluginFormatLifecyclePhase
{
    Uninitialized,
    Initializing,
    Ready,
    ShuttingDown,
    Shutdown,
};

class ObsJucePluginFormatLifecycle
{
public:
    static ObsJucePluginFormatLifecycle& getInstance();

    bool initialize();
    bool startMessagePump(QObject* qtParent);
    void pumpPendingMessages();
    void shutdown();

    ObsJucePluginFormatLifecyclePhase phase() const;
    bool isReady() const;
    bool isShuttingDown() const;

private:
    ObsJucePluginFormatLifecycle();
    ~ObsJucePluginFormatLifecycle();

    std::atomic<ObsJucePluginFormatLifecyclePhase> lifecyclePhase{ObsJucePluginFormatLifecyclePhase::Uninitialized};

    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juceRuntime;
    std::unique_ptr<MessagePump> messagePump;
};

} // namespace atk
