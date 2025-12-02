#include "PluginHost.h"

#include "../CpuMeter.h"
#include "../PluginHost2/Core/PluginGraph.h"
#include "Core/HostAudioProcessor.h"
#include "Core/PluginHolder.h"
#include "UI/HostEditorWindow.h"
#include "SecondaryThreadPool.h"
#include <atkaudio/FifoBuffer.h>
#include <chrono>

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
    // Async update flags (can be combined)
    enum UpdateFlags
    {
        None = 0,
        PrepareProcessor = 1 << 0,
        RestoreState = 1 << 1
    };

    // Pending async update flags (accessed from message thread only)
    std::atomic<int> pendingUpdateFlags{UpdateFlags::None};
    std::atomic<int> pendingChannels{0};
    std::atomic<int> pendingSamples{0};
    std::atomic<double> pendingSampleRate{0.0};
    std::string pendingStateString;
    juce::CriticalSection pendingStateLock; // Protect string from concurrent access

    // Track threading mode to detect transitions
    bool wasUsingThreading = false;

    // Job context for FIFO-based audio processing
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

    // Worker thread job for FIFO-based MT mode processing
    // Called by SecondaryThreadPool worker thread, NOT the OBS audio thread.
    // Processes ALL available data from input FIFOs and writes to output FIFOs.
    // processBlock() is called here, which:
    //   - Processes audio through the loaded plugin
    //   - Pushes to device outputs via routing matrix (immediately, no latency)
    //   - Pushes processed audio to output FIFO for OBS (one frame latency)
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

        // Try to acquire locks (non-blocking to avoid deadlock with UI thread)
        if (!impl->mainWindow->getPluginHolderLock().tryEnter())
        {
            context->markCompleted();
            return;
        }

        auto* processor = impl->mainWindow->getAudioProcessor();
        if (!processor->getCallbackLock().tryEnter())
        {
            impl->mainWindow->getPluginHolderLock().exit();
            context->markCompleted();
            return;
        }

        // Process all available data at once from the input FIFOs
        const int available = impl->inputFifo.getNumReady();
        if (available > 0)
        {
            auto& workBuffer = impl->workerBuffer;
            auto& workMidi = impl->workerMidiBuffer;

            // Prepare work buffer for all available samples
            workBuffer.setSize(workBuffer.getNumChannels(), available, false, false, true);

            // Read all available data from input FIFO
            for (int ch = 0; ch < workBuffer.getNumChannels(); ++ch)
            {
                bool isLastChannel = (ch == workBuffer.getNumChannels() - 1);
                impl->inputFifo.read(workBuffer.getWritePointer(ch), ch, available, isLastChannel);
            }

            // Clear MIDI (not implemented yet for FIFO-based approach)
            workMidi.clear();

            // Process audio through plugin
            if (!processor->isSuspended())
            {
                impl->cpuMeter.start();
                processor->processBlock(workBuffer, workMidi);
                impl->cpuMeter.stop(available, impl->sampleRate);
            }

            // Write all processed data to output FIFO
            for (int ch = 0; ch < workBuffer.getNumChannels(); ++ch)
            {
                bool isLastChannel = (ch == workBuffer.getNumChannels() - 1);
                impl->outputFifo.write(workBuffer.getReadPointer(ch), ch, available, isLastChannel);
            }
        }

        processor->getCallbackLock().exit();
        impl->mainWindow->getPluginHolderLock().exit();

        context->markCompleted();
    }

    Impl()
        : mainWindow(
              std::make_unique<HostEditorWindow>(
                  "atkAudio PluginHost",
                  juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                  createPluginHolder(),
                  [this]() { return useThreadPool.load(std::memory_order_acquire); },
                  [this](bool enabled) { setMultiCoreEnabled(enabled); }
              )
          )
    {
        // Initialize job context (starts as completed)
        jobContext.owner = this;

        // Set callbacks on the HostAudioProcessorImpl so the UI can access them
        if (auto* hostProc = mainWindow->getHostProcessor())
        {
            hostProc->getMultiCoreEnabled = [this]() { return useThreadPool.load(std::memory_order_acquire); };
            hostProc->setMultiCoreEnabled = [this](bool enabled) { setMultiCoreEnabled(enabled); };
            hostProc->getCpuLoad = [this]() { return getCpuLoad(); };
            hostProc->getLatencyMs = [this]() { return getLatencyMs(); };
        }

        // Ensure window starts hidden (exactly like PluginHost2 does)
        mainWindow->setVisible(false);

        // Thread pool will be initialized lazily in prepareProcessor
        // to avoid blocking constructor and prevent MessageManager deadlock
    }

    ~Impl()
    {
        // Cancel any pending async updates
        cancelPendingUpdate();

        // CRITICAL: Acquire timer mutex to wait for any in-flight callback to complete
        std::lock_guard<std::mutex> lock(timerMutex);

        // Clear callbacks
        if (mainWindow)
        {
            if (auto* hostProc = mainWindow->getHostProcessor())
            {
                hostProc->getMultiCoreEnabled = nullptr;
                hostProc->setMultiCoreEnabled = nullptr;
                hostProc->getCpuLoad = nullptr;
                hostProc->getLatencyMs = nullptr;
            }
        }

        // CRITICAL: Wait for any pending worker thread job to complete
        // This prevents crashes during destruction
        // Use timeout to avoid hanging if worker is stuck (e.g., plugin freeze)
        if (useThreadPool.load(std::memory_order_acquire) && !jobContext.isCompleted())
        {
            const auto timeout = std::chrono::milliseconds(100);
            jobContext.waitForCompletionWithTimeout(timeout);
        }

        // Safety check: If MessageManager is gone, we're in global shutdown
        // Don't try to clean up JUCE objects as this will crash
        if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
        {
            // Just leak the window - we're shutting down anyway
            mainWindow.release();
            return;
        }

        // Schedule async deletion - safe because callbacks are cleared
        auto* window = mainWindow.release();
        juce::MessageManager::callAsync([window]() { delete window; });
    }

    void handleAsyncUpdate() override
    {
        // Called on message thread to handle async updates safely
        auto flags = pendingUpdateFlags.exchange(UpdateFlags::None);

        // Handle PrepareProcessor first (must happen before state restore)
        if (flags & UpdateFlags::PrepareProcessor)
        {
            int channels = pendingChannels.load();
            int samples = pendingSamples.load();
            double rate = pendingSampleRate.load();
            prepareProcessorOnMessageThread(channels, samples, rate);
        }

        // Handle RestoreState after preparation
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
        juce::ScopedLock lock(mainWindow->getPluginHolderLock());
        auto* processor = mainWindow->getAudioProcessor();
        juce::ScopedLock callbackLock(processor->getCallbackLock());

        // Release previous configuration if needed
        if (isPrepared)
            processor->releaseResources();

        // Configure bus layout: main bus + sidechain bus
        auto layout = juce::AudioProcessor::BusesLayout();
        layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        layout.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));

        // Apply layout and prepare
        if (processor->checkBusesLayoutSupported(layout))
        {
            processor->setBusesLayout(layout);
            processor->setRateAndBufferSizeDetails(rate, samples);
            processor->prepareToPlay(rate, samples);

            // Initialize FIFOs for MT mode (8192 samples = ~170ms at 48kHz)
            const int fifoSize = 8192; // although 1024 might be enough
            inputFifo.setSize(channels * 2, fifoSize);
            outputFifo.setSize(channels * 2, fifoSize);
            workerBuffer.setSize(channels * 2, fifoSize); // Allocate for max FIFO size
            workerMidiBuffer.ensureSize(fifoSize);

            isPrepared = true;
        }
    }

    void process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
    {
        if (!buffer)
            return;

        // Check if we need to reconfigure the processor
        // Note: numSamples can vary between calls, only reallocate if we need MORE space
        bool needsReconfiguration =
            !isPrepared || numChannels != newNumChannels || numSamples < newNumSamples || sampleRate != newSampleRate;

        if (needsReconfiguration)
        {
            numChannels = newNumChannels;
            numSamples = juce::jmax(numSamples, newNumSamples); // Allocate for the largest size seen
            sampleRate = newSampleRate;

            // Resize sync-mode buffer
            syncBuffer.setSize(newNumChannels * 2, numSamples, false, false, true);
            syncMidiBuffer.ensureSize(numSamples);

            // Resize MT-mode FIFOs (will be properly initialized in prepareProcessorOnMessageThread)
            // No immediate action needed here - FIFOs are allocated in prepare

            // Schedule async preparation on message thread (can't block OBS audio thread)
            pendingChannels.store(newNumChannels);
            pendingSamples.store(numSamples);
            pendingSampleRate.store(newSampleRate);
            pendingUpdateFlags.fetch_or(UpdateFlags::PrepareProcessor);
            triggerAsyncUpdate();

            // Skip this buffer while preparing
            return;
        }

        // Skip processing if not yet prepared
        if (!isPrepared)
            return;

        //======================================================================
        // PROCESSING MODE SELECTION: MT (multi-threaded) or SYNC (synchronous)
        // These are mutually exclusive - only one path executes per callback.
        //======================================================================

        auto* pool = atk::SecondaryThreadPool::getInstance();
        const bool canUseThreading = useThreadPool.load(std::memory_order_acquire) && pool && pool->isReady();

        if (canUseThreading)
        {
            //==================================================================
            // MT MODE: FIFO-based processing on worker thread
            //
            // Pipeline:
            //   1. Wait for previous worker job to complete
            //   2. Push new input audio to input FIFO
            //   3. Pop processed audio from output FIFO to OBS buffer
            //   4. Kick worker to process all available FIFO data
            //==================================================================
            std::lock_guard<std::mutex> mtLock(mtProcessMutex);

            // Transition from sync to MT: initialize FIFO state
            if (!wasUsingThreading)
            {
                DBG("PluginHost: Enabling MT mode (FIFO-based)");
                inputFifo.reset();
                outputFifo.reset();
                workerBuffer.clear();
                workerMidiBuffer.clear();
                wasUsingThreading = true;
            }

            // Step 1: Wait for previous worker job with timeout (half frame time)
            // This balances latency vs glitch-free operation
            if (!jobContext.isCompleted())
            {
                const double frameTimeSeconds = newNumSamples / sampleRate;
                const double halfFrameTimeUs = frameTimeSeconds * 1000000.0 / 2.0;
                const auto timeout = std::chrono::microseconds(static_cast<long long>(halfFrameTimeUs));

                std::unique_lock<std::mutex> lock(jobContext.mutex);
                jobContext.cv
                    .wait_for(lock, timeout, [this] { return jobContext.completed.load(std::memory_order_acquire); });
            }

            // Step 2: Push new input audio to input FIFO
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
            {
                bool isLastChannel = (ch == newNumChannels * 2 - 1);
                inputFifo.write(buffer[ch], ch, newNumSamples, isLastChannel);
            }

            // Step 3: Pop processed audio from output FIFO to OBS buffer
            const int available = outputFifo.getNumReady();
            const int toRead = juce::jmin(available, newNumSamples);

            // Read what's available from output FIFO, pad with zeros if needed
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
            {
                bool isLastChannel = (ch == newNumChannels * 2 - 1);
                if (toRead > 0)
                    outputFifo.read(buffer[ch], ch, toRead, isLastChannel);
                if (toRead < newNumSamples)
                    juce::FloatVectorOperations::clear(buffer[ch] + toRead, newNumSamples - toRead);
            }

            // Step 4: Kick worker to process available input FIFO data (if not already busy)
            if (jobContext.isCompleted())
            {
                jobContext.reset();
                pool->addJob(&Impl::executeProcessJob, &jobContext);
                pool->kickWorkers();
            }

            return; // MT path complete - do NOT fall through to sync path
        }

        //======================================================================
        // SYNC MODE: Direct in-place processing on OBS audio thread
        //
        // No double-buffering, no latency. processBlock() is called directly
        // with the OBS buffer. Device outputs and OBS output are in sync.
        //======================================================================

        // Transition from MT to sync
        if (wasUsingThreading)
        {
            // Wait for worker to finish before switching modes
            if (!jobContext.isCompleted())
                jobContext.waitForCompletion();
            DBG("PluginHost: Disabling MT mode");
            wasUsingThreading = false;
        }

        // Try to acquire locks (non-blocking to avoid audio glitches)
        if (!mainWindow->getPluginHolderLock().tryEnter())
        {
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
                juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            return;
        }

        auto* processor = mainWindow->getAudioProcessor();
        if (!processor->getCallbackLock().tryEnter())
        {
            mainWindow->getPluginHolderLock().exit();
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
                juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            return;
        }

        // Process in-place: syncBuffer references the OBS buffer directly
        syncBuffer.setDataToReferTo(buffer, newNumChannels * 2, newNumSamples);
        syncMidiBuffer.clear();

        if (!processor->isSuspended())
        {
            cpuMeter.start();
            processor->processBlock(syncBuffer, syncMidiBuffer);
            cpuMeter.stop(newNumSamples, sampleRate);
        }

        processor->getCallbackLock().exit();
        mainWindow->getPluginHolderLock().exit();
    }

    juce::Component* getWindowComponent()
    {
        return mainWindow.get();
    }

    HostAudioProcessorImpl* getHostProcessor() const
    {
        if (!mainWindow)
            return nullptr;
        return mainWindow->getHostProcessor();
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

    // some plugins dont export state if audio is not playing
    void getState(std::string& s)
    {
        juce::ScopedLock lock(mainWindow->getPluginHolderLock());
        auto* processor = mainWindow->getAudioProcessor();
        juce::ScopedLock callbackLock(processor->getCallbackLock());
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        auto stateString = state.toString().toStdString();

        // Wrap plugin state with host settings (multi-core enabled flag)
        // Format: "MULTICORE:<0|1>\n<plugin_state>"
        bool multiEnabled = useThreadPool.load(std::memory_order_acquire);
        s = "MULTICORE:" + std::string(multiEnabled ? "1" : "0") + "\n" + stateString;
    }

    void setState(std::string& s)
    {
        if (s.empty())
            return;

        // Parse wrapped state format
        std::string pluginState = s;
        bool multiEnabled = false;

        // Check for wrapped format
        if (s.rfind("MULTICORE:", 0) == 0)
        {
            size_t newlinePos = s.find('\n');
            if (newlinePos != std::string::npos)
            {
                std::string header = s.substr(0, newlinePos);
                multiEnabled = (header.find(":1") != std::string::npos);
                pluginState = s.substr(newlinePos + 1);

                // Restore multi-core setting (pool initialized lazily in setMultiCoreEnabled)
                setMultiCoreEnabled(multiEnabled);
            }
        }

        // Defer state restoration to ensure plugin list and formats are fully initialized
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

        juce::ScopedLock lock(mainWindow->getPluginHolderLock());
        auto* processor = mainWindow->getAudioProcessor();
        juce::ScopedLock callbackLock(processor->getCallbackLock());
        juce::MemoryBlock stateData(stateString.data(), stateString.size());
        processor->setStateInformation(stateData.getData(), (int)stateData.getSize());
    }

    bool isSidechainEnabled() const
    {
        if (!mainWindow)
            return false;

        auto* processor = dynamic_cast<HostAudioProcessorImpl*>(mainWindow->getAudioProcessor());
        if (!processor)
            return false;

        return processor->isSidechainEnabled();
    }

    void setSidechainEnabled(bool enabled)
    {
        if (!mainWindow)
            return;

        auto* processor = dynamic_cast<HostAudioProcessorImpl*>(mainWindow->getAudioProcessor());
        if (!processor)
            return;

        processor->setSidechainEnabled(enabled);
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
            // ENABLING: Initialize dedicated thread pool (lazy create + init)
            auto* pool = atk::SecondaryThreadPool::getInstance();
            pool->initialize();
        }
        else if (!enabled && wasEnabled)
        {
            // DISABLING: Wait for any pending job to complete with timeout
            // Use timeout to avoid hanging UI if worker is stuck
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
        return cpuMeter.getLoad();
    }

    int getLatencyMs() const
    {
        std::lock_guard<std::mutex> lock(timerMutex);
        if (!mainWindow || sampleRate <= 0.0)
            return 0;

        auto* hostProc = mainWindow->getHostProcessor();
        if (!hostProc)
            return 0;

        auto* innerPlugin = hostProc->getInnerPlugin();
        if (!innerPlugin)
            return 0;

        int latencySamples = innerPlugin->getLatencySamples();

        bool mtEnabled = useThreadPool.load(std::memory_order_acquire);

        // In MT mode, add output FIFO samples to latency (samples waiting to be pulled)
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

        // Peak hold only in MT mode (FIFO latency fluctuates)
        if (!mtEnabled)
            return currentLatencyMs;

        return peakLatencyMs;
    }

private:
    std::unique_ptr<HostEditorWindow> mainWindow;

    // Sync mode: buffer references OBS buffer directly (setDataToReferTo)
    juce::AudioBuffer<float> syncBuffer;
    juce::MidiBuffer syncMidiBuffer;

    // MT mode: FIFO-based processing with worker thread
    atk::FifoBuffer inputFifo;             // OBS audio thread -> worker thread
    atk::FifoBuffer outputFifo;            // Worker thread -> OBS audio thread
    juce::AudioBuffer<float> workerBuffer; // Worker's processing buffer
    juce::MidiBuffer workerMidiBuffer;     // Worker's MIDI buffer
    ProcessJobContext jobContext;          // Single job context for worker

    int numChannels = 0;
    int numSamples = 0;
    double sampleRate = 0.0;

    bool isPrepared = false;
    std::atomic<bool> useThreadPool{false}; // Multi-core processing disabled by default

    // CPU meter for accurate load measurement with variable buffer sizes
    atk::CpuMeter cpuMeter;

    // Mutex to protect timer callbacks during destruction
    mutable std::mutex timerMutex;

    // Mutex to serialize MT processing when multiple sources call process() concurrently
    // (e.g., PluginHost as filter on Source Mixer with multiple audio sources)
    std::mutex mtProcessMutex;

    // Peak hold for latency display (3 second hold)
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

bool atk::PluginHost::isSidechainEnabled() const
{
    if (!pImpl)
        return false;

    return pImpl->isSidechainEnabled();
}

void atk::PluginHost::setSidechainEnabled(bool enabled)
{
    if (!pImpl)
        return;

    pImpl->setSidechainEnabled(enabled);
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

atk::PluginHost::PluginHost()
    : pImpl(new Impl())
{
}

atk::PluginHost::~PluginHost()
{
    delete pImpl;
}
