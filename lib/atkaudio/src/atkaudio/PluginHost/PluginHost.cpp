#include "PluginHost.h"

#include "../AudioProcessorGraphMT/AdaptiveSpinLock.h"
#include "../PluginHost2/Core/PluginGraph.h"
#include "Core/HostAudioProcessor.h"
#include "Core/PluginHolder.h"
#include "UI/HostEditorWindow.h"
#include "SecondaryThreadPool.h"

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

    // Job context for audio processing
    struct ProcessJobContext
    {
        Impl* owner = nullptr;
        int bufferIndex = 0;
        int numSamples = 0;
        std::atomic<bool> completed{false};

        void reset()
        {
            owner = nullptr;
            bufferIndex = 0;
            numSamples = 0;
            completed.store(false, std::memory_order_release);
        }
    };

    // Job execution on SecondaryThreadPool worker
    static void executeProcessJob(void* userData)
    {
        auto* context = static_cast<ProcessJobContext*>(userData);
        if (!context || !context->owner)
        {
            // Still mark as completed to prevent infinite wait
            if (context)
                context->completed.store(true, std::memory_order_release);
            return;
        }

        auto* impl = context->owner;

        // Try to acquire locks on worker thread (non-blocking, realtime-safe)
        if (!impl->mainWindow->getPluginHolderLock().tryEnter())
        {
            // Mark as completed even if we skip (output silence this frame)
            context->completed.store(true, std::memory_order_release);
            return;
        }

        auto* processor = impl->mainWindow->getAudioProcessor();
        if (!processor->getCallbackLock().tryEnter())
        {
            impl->mainWindow->getPluginHolderLock().exit();
            // Mark as completed even if we skip (output silence this frame)
            context->completed.store(true, std::memory_order_release);
            return;
        }

        // Process audio on worker thread using dedicated multi-core buffers
        auto& buffer = impl->multiCoreBuffers[context->bufferIndex];
        auto& midi = impl->multiCoreMidiBuffers[context->bufferIndex];

        // Ensure correct size
        buffer.setSize(buffer.getNumChannels(), context->numSamples, true, false, true);

        if (!processor->isSuspended())
            processor->processBlock(buffer, midi);

        // Release locks
        processor->getCallbackLock().exit();
        impl->mainWindow->getPluginHolderLock().exit();

        // Mark as completed
        context->completed.store(true, std::memory_order_release);
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
        // Initialize double buffer contexts
        jobContexts[0].reset();
        jobContexts[1].reset();

        // Set multi-core callbacks on the HostAudioProcessorImpl so the UI can access them
        if (auto* hostProc = mainWindow->getHostProcessor())
        {
            hostProc->getMultiCoreEnabled = [this]() { return useThreadPool.load(std::memory_order_acquire); };
            hostProc->setMultiCoreEnabled = [this](bool enabled) { setMultiCoreEnabled(enabled); };
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

        // CRITICAL: Wait for any pending worker thread jobs to complete
        // This prevents crashes during destruction
        // Note: Destructor is NOT on audio thread, so yield/sleep are acceptable
        if (useThreadPool.load(std::memory_order_acquire))
        {
            // Wait for both jobs to complete with timeout
            for (int i = 0; i < 2; ++i)
            {
                if (jobContexts[i].owner == nullptr)
                    continue; // No active job

                // Wait up to 100ms for completion
                const int maxWaitMs = 100;
                const int sleepMs = 1;
                int elapsed = 0;

                while (!jobContexts[i].completed.load(std::memory_order_acquire))
                {
                    if (elapsed >= maxWaitMs)
                        break; // Timeout - give up to prevent hang

                    juce::Thread::sleep(sleepMs);
                    elapsed += sleepMs;
                }
            }
        }

        // Safety check: If MessageManager is gone, we're in global shutdown
        // Don't try to clean up JUCE objects as this will crash
        if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
        {
            // Just leak the window - we're shutting down anyway
            mainWindow.release();
            return;
        }

        // IMPORTANT: All JUCE plugin operations MUST happen on the message thread
        // VST3 plugins especially require this (JUCE_ASSERT_MESSAGE_THREAD)
        // Just schedule the entire cleanup asynchronously
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

            // Configure adaptive spin lock for realtime-safe waiting
            spinLock.configure(samples, rate);

            // Thread pool initialized in atk::create()
            threadPoolInitialized = true;

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

            // Resize both double buffers (for single-core and as fallback)
            for (int i = 0; i < 2; ++i)
            {
                audioBuffers[i].setSize(newNumChannels * 2, numSamples, false, false, true);
                midiBuffers[i].ensureSize(numSamples);
                jobContexts[i].reset(); // Reset job contexts on reconfiguration
            }

            // Allocate dedicated multi-core buffers (always owned, never references)
            for (int i = 0; i < 2; ++i)
            {
                multiCoreBuffers[i].setSize(newNumChannels * 2, numSamples, false, true, true);
                multiCoreMidiBuffers[i].ensureSize(numSamples);
            }

            // Schedule async preparation (can't block OBS audio thread)
            // Note: This is different from standalone JUCE apps where audioDeviceAboutToStart
            // is called before audio begins. In OBS plugins, we must handle dynamic changes.
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

        // Multi-threading using dedicated PluginHost thread pool
        auto* pool = atk::SecondaryThreadPool::getInstance();
        const bool canUseThreading = useThreadPool.load(std::memory_order_acquire) && pool && pool->isReady();

        if (canUseThreading)
        {
            // If transitioning from sync to async, reset double-buffer state
            if (!wasUsingThreading)
            {
                jobContexts[0].reset();
                jobContexts[1].reset();
                currentBufferIndex = 0;
                // Mark context[0] as completed so first frame outputs it
                jobContexts[0].completed.store(true, std::memory_order_release);
                wasUsingThreading = true;
            }

            // DOUBLE-BUFFER PIPELINE (one frame latency):
            // Frame N:   Copy input → Check prev job → Output prev result → Submit current for processing
            // Frame N+1: Copy input → Check prev job → Output prev result → Submit current for processing

            // Step 1: Copy input to current buffer
            currentBufferIndex = 1 - currentBufferIndex;
            auto& currentBuffer = multiCoreBuffers[currentBufferIndex];
            auto& currentMidi = multiCoreMidiBuffers[currentBufferIndex];

            // Ensure buffer is large enough
            if (currentBuffer.getNumChannels() < newNumChannels * 2 || currentBuffer.getNumSamples() < newNumSamples)
            {
                int newCh = juce::jmax(currentBuffer.getNumChannels(), newNumChannels * 2);
                int newSmp = juce::jmax(currentBuffer.getNumSamples(), newNumSamples);
                currentBuffer.setSize(newCh, newSmp, true, true, true);
            }

            for (int ch = 0; ch < newNumChannels * 2 && ch < currentBuffer.getNumChannels(); ++ch)
                juce::FloatVectorOperations::copy(currentBuffer.getWritePointer(ch), buffer[ch], newNumSamples);
            currentMidi.clear();

            // Step 2: Check if previous job completed
            const int prevIndex = 1 - currentBufferIndex;
            auto& prevContext = jobContexts[prevIndex];

            bool prevCompleted = prevContext.completed.load(std::memory_order_acquire);

            if (prevCompleted)
            {
                // Previous frame completed - output it
                auto& prevBuffer = multiCoreBuffers[prevIndex];
                for (int ch = 0; ch < newNumChannels * 2 && ch < prevBuffer.getNumChannels(); ++ch)
                    juce::FloatVectorOperations::copy(buffer[ch], prevBuffer.getReadPointer(ch), newNumSamples);
            }
            else
            {
                // First frame or previous job still running - output silence
                for (int ch = 0; ch < newNumChannels * 2; ++ch)
                    juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            }

            // Step 3: Submit current buffer for async processing
            auto& context = jobContexts[currentBufferIndex];
            context.reset();
            context.owner = this;
            context.bufferIndex = currentBufferIndex;
            context.numSamples = newNumSamples;

            // Submit to SecondaryThreadPool
            pool->addJob(&Impl::executeProcessJob, &context);
            pool->kickWorkers();

            return; // Done with async path
        }

        // SYNCHRONOUS PROCESSING (only when MT is disabled)
        // Mark that we're not using threading (for transition detection)
        wasUsingThreading = false;

        if (!mainWindow->getPluginHolderLock().tryEnter())
        {
            // Can't get lock - output silence
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
                juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            return;
        }

        auto* processor = mainWindow->getAudioProcessor();

        if (!processor->getCallbackLock().tryEnter())
        {
            mainWindow->getPluginHolderLock().exit();
            // Can't get lock - output silence
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
                juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            return;
        }

        // Process audio in-place
        auto& currentBuffer = audioBuffers[currentBufferIndex];
        auto& currentMidi = midiBuffers[currentBufferIndex];

        currentBuffer.setDataToReferTo(buffer, newNumChannels * 2, newNumSamples);
        currentMidi.clear();

        if (!processor->isSuspended())
            processor->processBlock(currentBuffer, currentMidi);

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

                // Restore multi-core setting
                useThreadPool.store(multiEnabled, std::memory_order_release);
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
            // ENABLING: Initialize dedicated thread pool
            auto* pool = atk::SecondaryThreadPool::getInstance();
            if (!pool->isReady())
                pool->initialize();
        }
        else if (!enabled && wasEnabled)
        {
            // DISABLING: Wait for any pending jobs to complete

            for (int i = 0; i < 2; ++i)
            {
                if (jobContexts[i].owner == nullptr)
                    continue;

                const int maxWaitMs = 100;
                const int sleepMs = 1;
                int elapsed = 0;

                while (!jobContexts[i].completed.load(std::memory_order_acquire))
                {
                    if (elapsed >= maxWaitMs)
                        break;

                    juce::Thread::sleep(sleepMs);
                    elapsed += sleepMs;
                }

                // Clear the context
                jobContexts[i].owner = nullptr;
            }
        }
    }

private:
    std::unique_ptr<HostEditorWindow> mainWindow;

    // Separate buffer sets for single-core and multi-core modes
    // Single-core uses audioBuffers[0] as a reference (setDataToReferTo)
    // Multi-core uses multiCoreBuffers[0] and [1] as owned double-buffers
    juce::AudioBuffer<float> audioBuffers[2];     // Used by single-core (buffer 0) and as fallback
    juce::AudioBuffer<float> multiCoreBuffers[2]; // Dedicated owned buffers for multi-core
    juce::MidiBuffer midiBuffers[2];
    juce::MidiBuffer multiCoreMidiBuffers[2];

    // Job contexts for double-buffering
    ProcessJobContext jobContexts[2];

    // Adaptive spin lock for waiting on job completion (realtime-safe)
    atk::AdaptiveSpinLock spinLock;

    // Double-buffer index (toggles between 0 and 1)
    int currentBufferIndex = 0;

    int numChannels = 0;
    int numSamples = 0;
    double sampleRate = 0.0;

    bool isPrepared = false;
    std::atomic<bool> useThreadPool{false}; // Multi-core processing disabled by default
    bool threadPoolInitialized = false;     // Lazy initialization flag
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

atk::PluginHost::PluginHost()
    : pImpl(new Impl())
{
}

atk::PluginHost::~PluginHost()
{
    delete pImpl;
}
