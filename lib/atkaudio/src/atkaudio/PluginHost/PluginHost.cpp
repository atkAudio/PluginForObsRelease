#include "PluginHost.h"
#include "../PluginHost2/Core/PluginGraph.h"
#include "Core/HostAudioProcessor.h"
#include "Core/PluginHolder.h"
#include "UI/HostEditorWindow.h"
#include "SecondaryThreadPool.h"
#include <atkaudio/FifoBuffer.h>
#include <chrono>

#include "UI/QtDockWidget.h"
#include <obs-frontend-api.h>
#include <QWidget>
#include <QThread>
#include <QCoreApplication>
#include <QMetaObject>

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
            [this]() { mainComponent->recreateUI(); },
            [this]() { mainComponent->destroyUI(); }
        );
        qtWidget->setWindowTitle("atkAudio PluginHost");
        qtWidget->setConstrainerGetter([this]() { return mainComponent->getEditorConstrainer(); });

        mainComponent->setIsDockedCallback([this]() { return qtWidget->isDocked(); });
        qtWidget->setDockStateChangedCallback([this](bool isDocked) { mainComponent->setFooterVisible(!isDocked); });

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

        if (dockId && !obsExiting)
        {
            // obs_frontend_remove_dock() destroys Qt widgets - must run on main thread
            const char* dockIdToRemove = dockId;
            auto removeDock = [dockIdToRemove]() { obs_frontend_remove_dock(dockIdToRemove); };

            if (QThread::currentThread() == QCoreApplication::instance()->thread())
            {
                removeDock();
            }
            else
            {
                // We're on a background thread - invoke synchronously on the main thread
                QMetaObject::invokeMethod(QCoreApplication::instance(), removeDock, Qt::BlockingQueuedConnection);
            }

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

        auto* pool = atk::SecondaryThreadPool::getInstance();
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
                pool->addJob(&Impl::executeProcessJob, &jobContext);
                pool->kickWorkers();
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
        juce::ScopedLock lock(mainComponent->getPluginHolderLock());
        auto* processor = mainComponent->getAudioProcessor();
        juce::ScopedLock callbackLock(processor->getCallbackLock());
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        auto stateString = state.toString().toStdString();

        bool multiEnabled = useThreadPool.load(std::memory_order_acquire);
        s = "MULTICORE:"
          + std::string(multiEnabled ? "1" : "0")
          + "\n"
          + "DOCKVISIBLE:"
          + std::string(dockVisible ? "1" : "0")
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

                if (restoreDockVisible)
                    setVisible(true);
            }
        }

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
            auto* pool = atk::SecondaryThreadPool::getInstance();
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
