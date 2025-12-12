#include "PluginHost.h"
#include "../PluginHost2/Core/PluginGraph.h"
#include "Core/HostAudioProcessor.h"
#include "Core/PluginHolder.h"
#include "UI/HostEditorWindow.h"
#include "../AudioProcessorGraphMT/AudioThreadPool.h"
#include <atkaudio/FifoBuffer.h>
#include <chrono>

#include "UI/QtDockWidget.h"
#include <obs-frontend-api.h>
#include <QWidget>
#include <QThread>
#include <QCoreApplication>
#include <QMetaObject>
#include <QDockWidget>
#include <QMainWindow>
#include <QTimer>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HostAudioProcessor();
}

static std::unique_ptr<PluginHolder> createPluginHolder()
{
    return std::make_unique<PluginHolder>(nullptr);
}

struct atk::PluginHost::Impl : public juce::AsyncUpdater
{
    enum UpdateFlags
    {
        None = 0,
        PrepareProcessor = 1 << 0,
        RestoreState = 1 << 1
    };

    std::atomic<int> pendingUpdateFlags{UpdateFlags::None};
    std::atomic<int> pendingChannels{0};
    std::atomic<int> pendingSamples{0};
    std::atomic<double> pendingSampleRate{0.0};
    std::string pendingStateString;
    juce::CriticalSection pendingStateLock;

    bool wasUsingThreading = false;

    struct ProcessJobContext
    {
        Impl* owner = nullptr;
        std::atomic<bool> completed{true}; // Start as completed (no pending job)
        std::mutex mutex;
        std::condition_variable cv;

        void reset()
        {
            completed.store(false, std::memory_order_release);
        }

        void markCompleted()
        {
            std::lock_guard<std::mutex> lock(mutex);
            completed.store(true, std::memory_order_release);
            cv.notify_one();
        }

        void waitForCompletion()
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return completed.load(std::memory_order_acquire); });
        }

        bool waitForCompletionWithTimeout(std::chrono::microseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex);
            return cv.wait_for(lock, timeout, [this] { return completed.load(std::memory_order_acquire); });
        }

        bool isCompleted() const
        {
            return completed.load(std::memory_order_acquire);
        }
    };

    static void executeProcessJob(void* userData)
    {
        auto* context = static_cast<ProcessJobContext*>(userData);
        if (!context || !context->owner)
        {
            if (context)
                context->markCompleted();
            return;
        }

        auto* impl = context->owner;

        if (!impl->mainComponent->getPluginHolderLock().tryEnter())
        {
            context->markCompleted();
            return;
        }

        auto* processor = impl->mainComponent->getAudioProcessor();
        if (!processor->getCallbackLock().tryEnter())
        {
            impl->mainComponent->getPluginHolderLock().exit();
            context->markCompleted();
            return;
        }

        const int available = impl->inputFifo.getNumReady();
        if (available > 0)
        {
            auto& workBuffer = impl->workerBuffer;
            auto& workMidi = impl->workerMidiBuffer;

            workBuffer.setSize(workBuffer.getNumChannels(), available, false, false, true);

            for (int ch = 0; ch < workBuffer.getNumChannels(); ++ch)
            {
                bool isLastChannel = (ch == workBuffer.getNumChannels() - 1);
                impl->inputFifo.read(workBuffer.getWritePointer(ch), ch, available, isLastChannel);
            }

            workMidi.clear();

            if (!processor->isSuspended())
            {
                juce::AudioProcessLoadMeasurer::ScopedTimer timer(impl->loadMeasurer, available);
                processor->processBlock(workBuffer, workMidi);
            }

            for (int ch = 0; ch < workBuffer.getNumChannels(); ++ch)
            {
                bool isLastChannel = (ch == workBuffer.getNumChannels() - 1);
                impl->outputFifo.write(workBuffer.getReadPointer(ch), ch, available, isLastChannel);
            }
        }

        processor->getCallbackLock().exit();
        impl->mainComponent->getPluginHolderLock().exit();

        context->markCompleted();
    }

    static void frontendEventCallback(enum obs_frontend_event event, void* private_data)
    {
        auto* impl = static_cast<Impl*>(private_data);
        if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
            impl->obsExiting = true;
    }

    Impl()
        : mainComponent(new HostEditorComponent(createPluginHolder()))
    {
        jobContext.owner = this;

        if (auto* hostProc = mainComponent->getHostProcessor())
        {
            hostProc->getMultiCoreEnabled = [this]() { return useThreadPool.load(std::memory_order_acquire); };
            hostProc->setMultiCoreEnabled = [this](bool enabled) { setMultiCoreEnabled(enabled); };
            hostProc->getCpuLoad = [this]() { return getCpuLoad(); };
            hostProc->getLatencyMs = [this]() { return getLatencyMs(); };
        }

        obs_frontend_add_event_callback(frontendEventCallback, this);

        qtWidget = new atk::JuceQtWidget(
            mainComponent,
            [this]()
            {
                mainComponent->recreateUI();
                updateDockState();
            },
            [this]()
            {
                mainComponent->destroyUI();
                updateDockState();
            }
        );
        qtWidget->setWindowTitle("atkAudio PluginHost");
        qtWidget->setConstrainerGetter([this]() { return mainComponent->getEditorConstrainer(); });

        mainComponent->setIsDockedCallback([this]() { return qtWidget->isDocked(); });
        qtWidget->setDockStateChangedCallback(
            [this](bool isDocked)
            {
                mainComponent->setFooterVisible(!isDocked);
                updateDockState();
            }
        );

        mainComponent->destroyUI();
        mainComponent->setVisible(false);
    }

    ~Impl()
    {
        obs_frontend_remove_event_callback(frontendEventCallback, this);

        cancelPendingUpdate();

        std::lock_guard<std::mutex> lock(timerMutex);

        if (mainComponent)
        {
            if (auto* hostProc = mainComponent->getHostProcessor())
            {
                hostProc->getMultiCoreEnabled = nullptr;
                hostProc->setMultiCoreEnabled = nullptr;
                hostProc->getCpuLoad = nullptr;
                hostProc->getLatencyMs = nullptr;
            }
        }

        if (useThreadPool.load(std::memory_order_acquire) && !jobContext.isCompleted())
        {
            const auto timeout = std::chrono::milliseconds(100);
            jobContext.waitForCompletionWithTimeout(timeout);
        }

        if (qtWidget)
            qtWidget->clearCallbacks();

        if (dockId)
        {
            // obs_frontend_remove_dock() destroys Qt widgets - defer to Qt event loop
            // Capture string by value since dockIdStorage will be destroyed
            std::string dockIdCopy = dockIdStorage;
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [dockIdCopy]() { obs_frontend_remove_dock(dockIdCopy.c_str()); },
                Qt::QueuedConnection
            );

            dockId = nullptr;
            qtWidget = nullptr;
        }

        mainComponent = nullptr;
    }

    void handleAsyncUpdate() override
    {
        auto flags = pendingUpdateFlags.exchange(UpdateFlags::None);

        if (flags & UpdateFlags::PrepareProcessor)
        {
            int channels = pendingChannels.load();
            int samples = pendingSamples.load();
            double rate = pendingSampleRate.load();
            prepareProcessorOnMessageThread(channels, samples, rate);
        }

        if (flags & UpdateFlags::RestoreState)
        {
            std::string stateString;
            {
                juce::ScopedLock lock(pendingStateLock);
                stateString = std::move(pendingStateString);
            }
            restoreStateOnMessageThread(stateString);
        }
    }

    void prepareProcessorOnMessageThread(int channels, int samples, double rate)
    {
        // Wait for any background processing job to complete first
        if (!jobContext.isCompleted())
            jobContext.waitForCompletion();

        // Acquire locks - audio callback uses tryEnter so it will bail out
        juce::ScopedLock lock(mainComponent->getPluginHolderLock());
        auto* processor = mainComponent->getAudioProcessor();
        juce::ScopedLock callbackLock(processor->getCallbackLock());

        if (isPrepared)
            processor->releaseResources();

        auto layout = juce::AudioProcessor::BusesLayout();
        layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        layout.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));

        if (processor->checkBusesLayoutSupported(layout))
        {
            processor->setBusesLayout(layout);
            processor->setRateAndBufferSizeDetails(rate, samples);
            processor->prepareToPlay(rate, samples);

            loadMeasurer.reset(rate, samples);

            const int fifoSize = 8192;
            inputFifo.setSize(channels * 2, fifoSize);
            outputFifo.setSize(channels * 2, fifoSize);
            workerBuffer.setSize(channels * 2, fifoSize);
            workerMidiBuffer.ensureSize(fifoSize);

            isPrepared = true;
        }
    }

    void process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
    {
        if (!buffer)
            return;

        bool needsReconfiguration =
            !isPrepared || numChannels != newNumChannels || numSamples < newNumSamples || sampleRate != newSampleRate;

        if (needsReconfiguration)
        {
            numChannels = newNumChannels;
            numSamples = juce::jmax(numSamples, newNumSamples);
            sampleRate = newSampleRate;

            syncBuffer.setSize(newNumChannels * 2, numSamples, false, false, true);
            syncMidiBuffer.ensureSize(numSamples);

            pendingChannels.store(newNumChannels);
            pendingSamples.store(numSamples);
            pendingSampleRate.store(newSampleRate);
            pendingUpdateFlags.fetch_or(UpdateFlags::PrepareProcessor);
            triggerAsyncUpdate();

            return;
        }

        if (!isPrepared)
            return;

        auto* pool = atk::AudioThreadPool::getInstance();
        const bool canUseThreading = useThreadPool.load(std::memory_order_acquire) && pool && pool->isReady();

        if (canUseThreading)
        {
            std::lock_guard<std::mutex> mtLock(mtProcessMutex);

            if (!wasUsingThreading)
            {
                inputFifo.reset();
                outputFifo.reset();
                workerBuffer.clear();
                workerMidiBuffer.clear();
                wasUsingThreading = true;
            }

            if (!jobContext.isCompleted())
            {
                const double frameTimeSeconds = newNumSamples / sampleRate;
                const double halfFrameTimeUs = frameTimeSeconds * 1000000.0 / 2.0;
                const auto timeout = std::chrono::microseconds(static_cast<long long>(halfFrameTimeUs));

                std::unique_lock<std::mutex> lock(jobContext.mutex);
                jobContext.cv
                    .wait_for(lock, timeout, [this] { return jobContext.completed.load(std::memory_order_acquire); });
            }

            for (int ch = 0; ch < newNumChannels * 2; ++ch)
            {
                bool isLastChannel = (ch == newNumChannels * 2 - 1);
                inputFifo.write(buffer[ch], ch, newNumSamples, isLastChannel);
            }

            const int available = outputFifo.getNumReady();
            const int toRead = juce::jmin(available, newNumSamples);

            for (int ch = 0; ch < newNumChannels * 2; ++ch)
            {
                bool isLastChannel = (ch == newNumChannels * 2 - 1);
                if (toRead > 0)
                    outputFifo.read(buffer[ch], ch, toRead, isLastChannel);
                if (toRead < newNumSamples)
                    juce::FloatVectorOperations::clear(buffer[ch] + toRead, newNumSamples - toRead);
            }

            if (jobContext.isCompleted())
            {
                jobContext.reset();
                pool->submitTask(&Impl::executeProcessJob, &jobContext);
            }

            return;
        }

        if (wasUsingThreading)
        {
            if (!jobContext.isCompleted())
                jobContext.waitForCompletion();
            wasUsingThreading = false;
        }

        if (!mainComponent->getPluginHolderLock().tryEnter())
        {
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
                juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            return;
        }

        auto* processor = mainComponent->getAudioProcessor();
        if (!processor->getCallbackLock().tryEnter())
        {
            mainComponent->getPluginHolderLock().exit();
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
                juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            return;
        }

        syncBuffer.setDataToReferTo(buffer, newNumChannels * 2, newNumSamples);
        syncMidiBuffer.clear();

        if (!processor->isSuspended())
        {
            juce::AudioProcessLoadMeasurer::ScopedTimer timer(loadMeasurer, newNumSamples);
            processor->processBlock(syncBuffer, syncMidiBuffer);
        }

        processor->getCallbackLock().exit();
        mainComponent->getPluginHolderLock().exit();
    }

    void setVisible(bool visible)
    {
        dockVisible = visible;

        if (!qtWidget)
            return;

        if (visible)
        {
            if (!dockId && !dockIdStorage.empty())
            {
                dockId = dockIdStorage.c_str();
                obs_frontend_add_dock_by_id(dockId, "atkAudio PluginHost", qtWidget);
            }

            // Try to apply pending dock state immediately
            applyPendingDockState();

            // If we still have pending state and parent isn't ready yet, retry after a short delay
            // (parent dock might not be set immediately by OBS)
            if ((hasPendingDockFloating || hasPendingDockGeom || hasPendingDockArea) && !qtWidget->parentWidget())
                QTimer::singleShot(10, qtWidget, [this]() { applyPendingDockState(); });

            if (QWidget* parentDock = qtWidget->parentWidget())
            {
                parentDock->show();
                parentDock->raise();
                parentDock->activateWindow();
            }
            else
            {
                qtWidget->show();
            }
        }
        else
        {
            if (QWidget* parentDock = qtWidget->parentWidget())
                parentDock->hide();
            else
                qtWidget->hide();
        }
    }

    void applyPendingDockState()
    {
        if (!qtWidget)
            return;

        QWidget* parentDock = qtWidget->parentWidget();
        if (!parentDock)
            return;

        auto* dock = qobject_cast<QDockWidget*>(parentDock);
        if (!dock)
            return;

        // Connect to dock signals if not already connected
        if (!dockSignalsConnected)
            connectDockSignals(dock);

        // Apply pending floating state
        if (hasPendingDockFloating)
        {
            dock->setFloating(pendingDockFloating);
            hasPendingDockFloating = false;
        }

        // Apply geometry for floating windows
        if (hasPendingDockGeom && dock->isFloating())
        {
            if (pendingDockGeom.w > 0 && pendingDockGeom.h > 0)
                dock->setGeometry(pendingDockGeom.x, pendingDockGeom.y, pendingDockGeom.w, pendingDockGeom.h);
            hasPendingDockGeom = false;
        }

        // Apply dock area for docked windows
        if (hasPendingDockArea && !dock->isFloating())
        {
            if (auto* mainWin = qobject_cast<QMainWindow*>(static_cast<QWidget*>(obs_frontend_get_main_window())))
            {
                auto area = static_cast<Qt::DockWidgetArea>(pendingDockArea);
                mainWin->addDockWidget(area, dock);
            }
            hasPendingDockArea = false;
        }
    }

    void connectDockSignals(QDockWidget* dock)
    {
        if (!dock || dockSignalsConnected)
            return;

        // Initialize current state from the dock
        updateDockState();

        // Track any state changes from the dock widget
        QObject::connect(dock, &QDockWidget::visibilityChanged, qtWidget, [this](bool) { updateDockState(); });

        QObject::connect(dock, &QDockWidget::topLevelChanged, qtWidget, [this](bool) { updateDockState(); });

        dockSignalsConnected = true;
    }

    void updateDockState()
    {
        if (!qtWidget)
            return;

        // Query current state from the dock widget
        if (auto* dock = qobject_cast<QDockWidget*>(qtWidget->parentWidget()))
        {
            dockVisible = dock->isVisible();
            dockFloating = dock->isFloating();
        }
        else if (qtWidget->isVisible())
        {
            // Widget visible but no parent dock yet
            dockVisible = true;
            dockFloating = false;
        }
    }

    void setDockId(const std::string& id)
    {
        if (id.empty())
            return;

        dockIdStorage = "atkaudio_pluginhost_" + id;
    }

    bool isDockVisible() const
    {
        return dockVisible;
    }

    juce::Component* getWindowComponent()
    {
        return mainComponent;
    }

    HostAudioProcessorImpl* getHostProcessor() const
    {
        if (!mainComponent)
            return nullptr;
        return mainComponent->getHostProcessor();
    }

    int getInnerPluginChannelCount() const
    {
        auto* hostProc = getHostProcessor();
        if (!hostProc)
            return 2;

        auto* innerPlugin = hostProc->getInnerPlugin();
        if (!innerPlugin)
            return 2;

        return juce::jmax(innerPlugin->getTotalNumInputChannels(), innerPlugin->getTotalNumOutputChannels());
    }

    void getState(std::string& s)
    {
        juce::MemoryBlock state;

        {
            // Wait for any background processing job first
            if (!jobContext.isCompleted())
                jobContext.waitForCompletion();

            juce::ScopedLock lock(mainComponent->getPluginHolderLock());
            auto* processor = mainComponent->getAudioProcessor();
            juce::ScopedLock callbackLock(processor->getCallbackLock());
            processor->getStateInformation(state);
        }
        auto stateString = state.toString().toStdString();

        bool multiEnabled = useThreadPool.load(std::memory_order_acquire);

        // Get dock geometry info
        bool isFloating = dockFloating;
        int dockX = 0, dockY = 0, dockW = 0, dockH = 0;
        int dockArea = 0; // Qt::DockWidgetArea: 1=left, 2=right, 4=top, 8=bottom
        if (qtWidget)
        {
            if (auto* dock = qobject_cast<QDockWidget*>(qtWidget->parentWidget()))
            {
                // isFloating already set from dockFloating state
                auto geom = dock->geometry();
                dockX = geom.x();
                dockY = geom.y();
                dockW = geom.width();
                dockH = geom.height();

                if (!isFloating)
                {
                    if (auto* mainWin =
                            qobject_cast<QMainWindow*>(static_cast<QWidget*>(obs_frontend_get_main_window())))
                    {
                        dockArea = static_cast<int>(mainWin->dockWidgetArea(dock));
                    }
                }
            }
        }

        s = "MULTICORE:"
          + std::string(multiEnabled ? "1" : "0")
          + "\n"
          + "DOCKVISIBLE:"
          + std::string(dockVisible ? "1" : "0")
          + "\n"
          + "DOCKFLOATING:"
          + std::string(isFloating ? "1" : "0")
          + "\n"
          + "DOCKAREA:"
          + std::to_string(dockArea)
          + "\n"
          + "DOCKGEOM:"
          + std::to_string(dockX)
          + ","
          + std::to_string(dockY)
          + ","
          + std::to_string(dockW)
          + ","
          + std::to_string(dockH)
          + "\n"
          + stateString;
    }

    void setState(std::string& s)
    {
        if (s.empty())
            return;

        std::string pluginState = s;
        bool multiEnabled = false;
        bool restoreDockVisible = false;

        // Parse MULTICORE header
        if (pluginState.rfind("MULTICORE:", 0) == 0)
        {
            size_t newlinePos = pluginState.find('\n');
            if (newlinePos != std::string::npos)
            {
                std::string header = pluginState.substr(0, newlinePos);
                multiEnabled = (header.find(":1") != std::string::npos);
                pluginState = pluginState.substr(newlinePos + 1);
                setMultiCoreEnabled(multiEnabled);
            }
        }

        // Parse DOCKVISIBLE header
        if (pluginState.rfind("DOCKVISIBLE:", 0) == 0)
        {
            size_t newlinePos = pluginState.find('\n');
            if (newlinePos != std::string::npos)
            {
                std::string header = pluginState.substr(0, newlinePos);
                restoreDockVisible = (header.find(":1") != std::string::npos);
                pluginState = pluginState.substr(newlinePos + 1);
            }
        }

        // Parse DOCKFLOATING header
        if (pluginState.rfind("DOCKFLOATING:", 0) == 0)
        {
            size_t newlinePos = pluginState.find('\n');
            if (newlinePos != std::string::npos)
            {
                std::string header = pluginState.substr(0, newlinePos);
                pendingDockFloating = (header.find(":1") != std::string::npos);
                hasPendingDockFloating = true;
                pluginState = pluginState.substr(newlinePos + 1);
            }
        }

        // Skip legacy DOCKTAB header if present
        if (pluginState.rfind("DOCKTAB:", 0) == 0)
        {
            size_t newlinePos = pluginState.find('\n');
            if (newlinePos != std::string::npos)
                pluginState = pluginState.substr(newlinePos + 1);
        }

        // Parse DOCKAREA header
        if (pluginState.rfind("DOCKAREA:", 0) == 0)
        {
            size_t newlinePos = pluginState.find('\n');
            if (newlinePos != std::string::npos)
            {
                std::string header = pluginState.substr(9, newlinePos - 9); // Skip "DOCKAREA:"
                pluginState = pluginState.substr(newlinePos + 1);
                pendingDockArea = std::atoi(header.c_str());
                hasPendingDockArea = (pendingDockArea > 0);
            }
        }

        // Parse DOCKGEOM header
        if (pluginState.rfind("DOCKGEOM:", 0) == 0)
        {
            size_t newlinePos = pluginState.find('\n');
            if (newlinePos != std::string::npos)
            {
                std::string header = pluginState.substr(9, newlinePos - 9); // Skip "DOCKGEOM:"
                pluginState = pluginState.substr(newlinePos + 1);

                // Parse geometry: x,y,width,height
                int x = 0, y = 0, w = 0, h = 0;
                if (sscanf(header.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) == 4)
                {
                    pendingDockGeom = {x, y, w, h};
                    hasPendingDockGeom = true;
                }
            }
        }

        // Restore dock visibility
        if (restoreDockVisible)
            setVisible(true);

        {
            juce::ScopedLock lock(pendingStateLock);
            pendingStateString = pluginState;
        }
        pendingUpdateFlags.fetch_or(UpdateFlags::RestoreState);
        triggerAsyncUpdate();
    }

    void restoreStateOnMessageThread(const std::string& stateString)
    {
        if (stateString.empty())
            return;

        if (!jobContext.isCompleted())
            jobContext.waitForCompletion();

        juce::ScopedLock lock(mainComponent->getPluginHolderLock());
        auto* processor = mainComponent->getAudioProcessor();
        juce::ScopedLock callbackLock(processor->getCallbackLock());
        juce::MemoryBlock stateData(stateString.data(), stateString.size());
        processor->setStateInformation(stateData.getData(), (int)stateData.getSize());
    }

    bool isMultiCoreEnabled() const
    {
        return useThreadPool.load(std::memory_order_acquire);
    }

    void setMultiCoreEnabled(bool enabled)
    {
        bool wasEnabled = useThreadPool.exchange(enabled, std::memory_order_acq_rel);

        if (enabled && !wasEnabled)
        {
            auto* pool = atk::AudioThreadPool::getInstance();
            if (pool && !pool->isReady())
                pool->initialize();
        }
        else if (!enabled && wasEnabled)
        {
            if (!jobContext.isCompleted())
            {
                const auto timeout = std::chrono::milliseconds(100);
                jobContext.waitForCompletionWithTimeout(timeout);
            }
        }
    }

    float getCpuLoad() const
    {
        std::lock_guard<std::mutex> lock(timerMutex);
        float currentLoad = static_cast<float>(loadMeasurer.getLoadAsProportion());

        auto now = juce::Time::getMillisecondCounterHiRes();
        if (currentLoad >= peakCpuLoad)
        {
            peakCpuLoad = currentLoad;
            peakCpuTime = now;
        }
        else if (now - peakCpuTime > 3000.0)
        {
            peakCpuLoad = currentLoad;
            peakCpuTime = now;
        }

        return peakCpuLoad;
    }

    int getLatencyMs() const
    {
        std::lock_guard<std::mutex> lock(timerMutex);
        if (!mainComponent || sampleRate <= 0.0)
            return 0;

        auto* hostProc = mainComponent->getHostProcessor();
        if (!hostProc)
            return 0;

        auto* innerPlugin = hostProc->getInnerPlugin();
        if (!innerPlugin)
            return 0;

        int latencySamples = innerPlugin->getLatencySamples();

        bool mtEnabled = useThreadPool.load(std::memory_order_acquire);

        if (mtEnabled)
            latencySamples += outputFifo.getNumReady();

        int currentLatencyMs = 0;
        if (latencySamples > 0)
            currentLatencyMs = static_cast<int>(std::round(latencySamples / sampleRate * 1000.0));

        // Peak hold for 3 seconds
        auto now = std::chrono::steady_clock::now();
        if (currentLatencyMs >= peakLatencyMs)
        {
            peakLatencyMs = currentLatencyMs;
            peakLatencyTime = now;
        }
        else
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - peakLatencyTime).count();
            if (elapsed > 3000)
                peakLatencyMs = currentLatencyMs;
        }

        if (!mtEnabled)
            return currentLatencyMs;

        return peakLatencyMs;
    }

private:
    HostEditorComponent* mainComponent = nullptr;
    atk::JuceQtWidget* qtWidget = nullptr;
    const char* dockId = nullptr;
    std::string dockIdStorage;
    bool obsExiting = false;
    bool dockVisible = false;
    bool dockFloating = true; // Default to floating when no state is loaded
    bool dockSignalsConnected = false;
    bool pendingDockFloating = false;
    bool hasPendingDockFloating = false;

    struct DockGeom
    {
        int x, y, w, h;
    };

    DockGeom pendingDockGeom{};
    bool hasPendingDockGeom = false;
    int pendingDockArea = 0;
    bool hasPendingDockArea = false;

    juce::AudioBuffer<float> syncBuffer;
    juce::MidiBuffer syncMidiBuffer;

    atk::FifoBuffer inputFifo;
    atk::FifoBuffer outputFifo;
    juce::AudioBuffer<float> workerBuffer;
    juce::MidiBuffer workerMidiBuffer;
    ProcessJobContext jobContext;

    int numChannels = 0;
    int numSamples = 0;
    double sampleRate = 0.0;

    bool isPrepared = false;
    std::atomic<bool> useThreadPool{false};

    juce::AudioProcessLoadMeasurer loadMeasurer;

    mutable float peakCpuLoad = 0.0f;
    mutable double peakCpuTime = 0.0;

    mutable std::mutex timerMutex;

    std::mutex mtProcessMutex;

    mutable int peakLatencyMs = 0;
    mutable std::chrono::steady_clock::time_point peakLatencyTime;
};

void atk::PluginHost::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::PluginHost::getState(std::string& s)
{
    pImpl->getState(s);
}

void atk::PluginHost::setState(std::string& s)
{
    pImpl->setState(s);
}

juce::Component* atk::PluginHost::getWindowComponent()
{
    return pImpl->getWindowComponent();
}

int atk::PluginHost::getInnerPluginChannelCount() const
{
    if (!pImpl)
        return 2; // Default to stereo

    return pImpl->getInnerPluginChannelCount();
}

bool atk::PluginHost::isMultiCoreEnabled() const
{
    if (!pImpl)
        return false;

    return pImpl->isMultiCoreEnabled();
}

void atk::PluginHost::setMultiCoreEnabled(bool enabled)
{
    if (!pImpl)
        return;

    pImpl->setMultiCoreEnabled(enabled);
}

float atk::PluginHost::getCpuLoad() const
{
    if (!pImpl)
        return 0.0f;

    return pImpl->getCpuLoad();
}

int atk::PluginHost::getLatencyMs() const
{
    if (!pImpl)
        return 0;

    return pImpl->getLatencyMs();
}

void atk::PluginHost::setVisible(bool visible)
{
    if (!pImpl)
        return;

    auto doUi = [this, visible]() { pImpl->setVisible(visible); };

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        doUi();
    else
        juce::MessageManager::callAsync(doUi);
}

void atk::PluginHost::setDockId(const std::string& id)
{
    if (!pImpl)
        return;

    pImpl->setDockId(id);
}

bool atk::PluginHost::isDockVisible() const
{
    if (!pImpl)
        return false;

    return pImpl->isDockVisible();
}

atk::PluginHost::PluginHost()
    : pImpl(new Impl())
{
}

atk::PluginHost::~PluginHost()
{
    delete pImpl;
}
