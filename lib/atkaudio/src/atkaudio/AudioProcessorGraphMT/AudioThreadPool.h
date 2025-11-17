#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>
#include "AdaptiveSpinLock.h"

namespace atk
{

//==============================================================================
/**
    Job context for audio processing jobs.
    Fixed-size struct to avoid std::function allocations.
*/
struct AudioJobContext
{
    void* userData = nullptr;           // User-defined context pointer
    void (*execute)(void*) = nullptr;   // Function pointer to execute with userData
    std::atomic<bool> completed{false}; // Completion flag

    AudioJobContext()
        : userData(nullptr)
        , execute(nullptr)
        , completed(false)
    {
    }

    // Move constructor
    AudioJobContext(AudioJobContext&& other) noexcept
        : userData(other.userData)
        , execute(other.execute)
        , completed(other.completed.load(std::memory_order_acquire))
    {
        other.userData = nullptr;
        other.execute = nullptr;
    }

    // Move assignment
    AudioJobContext& operator=(AudioJobContext&& other) noexcept
    {
        if (this != &other)
        {
            userData = other.userData;
            execute = other.execute;
            completed.store(other.completed.load(std::memory_order_acquire), std::memory_order_release);
            other.userData = nullptr;
            other.execute = nullptr;
        }
        return *this;
    }

    // Delete copy operations
    AudioJobContext(const AudioJobContext&) = delete;
    AudioJobContext& operator=(const AudioJobContext&) = delete;

    bool isValid() const
    {
        return execute != nullptr;
    }

    void run()
    {
        if (execute && userData)
            execute(userData);
    }

    void reset()
    {
        userData = nullptr;
        execute = nullptr;
        completed.store(false, std::memory_order_release);
    }
};

//==============================================================================
/**
    Lock-free barrier for real-time audio thread synchronization.
*/
class ThreadBarrier : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<ThreadBarrier>;

    static Ptr make(int numThreadsToSynchronise)
    {
        return {new ThreadBarrier{numThreadsToSynchronise}};
    }

    void configure(int samplesPerBlock, double sampleRate)
    {
        (void)samplesPerBlock;
        (void)sampleRate;
    }

    void arriveAndWait()
    {
        const int myGeneration = generation.load(std::memory_order_acquire);
        const int arrivals = arrivalCount.fetch_add(1, std::memory_order_acq_rel) + 1;

        jassert(arrivals <= threadCount);

        if (arrivals == threadCount)
        {
            arrivalCount.store(0, std::memory_order_release);
            generation.fetch_add(1, std::memory_order_release);
            return;
        }

        spinLock.wait([this, myGeneration] { return generation.load(std::memory_order_acquire) != myGeneration; });
    }

private:
    explicit ThreadBarrier(int numThreadsToSynchronise)
        : threadCount(numThreadsToSynchronise)
        , arrivalCount(0)
        , generation(0)
        , spinLock() // Uses default Mode::Fixed8192Backoff
    {
    }

    const int threadCount;
    std::atomic<int> arrivalCount;
    std::atomic<int> generation;
    AdaptiveSpinLock spinLock;

    JUCE_DECLARE_NON_COPYABLE(ThreadBarrier)
    JUCE_DECLARE_NON_MOVEABLE(ThreadBarrier)
};

//==============================================================================
/**
    Lock-free job queue for parallel audio processing.
*/
class RealtimeJobQueue
{
public:
    using Job = AudioJobContext;

    RealtimeJobQueue()
        : nextJobIndex(0)
        , totalJobs(0)
    {
        jobs.resize(256);
    }

    void reset()
    {
        totalJobs.store(0, std::memory_order_release);
        nextJobIndex.store(0, std::memory_order_release);
    }

    size_t addJob(void (*execute)(void*), void* userData)
    {
        const size_t index = totalJobs.fetch_add(1, std::memory_order_acq_rel);
        jassert(index < jobs.size());

        jobs[index].execute = execute;
        jobs[index].userData = userData;
        jobs[index].completed.store(false, std::memory_order_release);

        return index;
    }

    bool tryClaimJob(Job*& outJob)
    {
        const int total = totalJobs.load(std::memory_order_acquire);
        const int jobIdx = nextJobIndex.fetch_add(1, std::memory_order_acq_rel);

        if (jobIdx >= total)
            return false;

        outJob = &jobs[jobIdx];
        return outJob->isValid();
    }

    int getRemainingJobs() const
    {
        const int total = totalJobs.load(std::memory_order_acquire);
        const int next = nextJobIndex.load(std::memory_order_acquire);
        return juce::jmax(0, total - next);
    }

    int getTotalJobs() const
    {
        return totalJobs.load(std::memory_order_acquire);
    }

    bool isJobCompleted(int index) const
    {
        if (index < 0 || index >= static_cast<int>(jobs.size()))
            return true;
        return jobs[index].completed.load(std::memory_order_acquire);
    }

private:
    std::vector<Job> jobs;
    std::atomic<int> nextJobIndex;
    std::atomic<int> totalJobs;

    JUCE_DECLARE_NON_COPYABLE(RealtimeJobQueue)
};

//==============================================================================
/**
    Global audio thread pool singleton - REALTIME SAFE AUDIO PATH.

    This thread pool is designed for realtime audio processing with the following features:
    - Singleton instance shared across all audio graph processors
    - Persistent worker threads to avoid creation/destruction overhead
    - Lock-free job queue for realtime-safe job distribution (atomic fetch_add)
    - Lock-free barrier synchronization using atomic operations + adaptive spinning
    - High-priority threads (not realtime scheduled, for stability)
    - AdaptiveSpinLock with capped exponential backoff (8→16→32→64→128→256→512→1024→2048→4096→8192→16384 pauses, then
   yield)
    - Uses DeletedAtShutdown for proper cleanup order

    Hybrid approach for optimal CPU usage:
    - WorkerThread idle wait (between callbacks): brief spin + sleep (low CPU)
    - ThreadBarrier (during audio processing): atomic counters + adaptive spinning (realtime-safe)
    - RealtimeJobQueue: atomic fetch_add for lock-free job claiming (realtime-safe)
    - Job processing: fully lock-free and realtime-safe

    Why hybrid?
    - Workers sleep between audio callbacks (50-100Hz rate) → low CPU when idle
    - Barrier synchronization during audio processing is fully realtime-safe
    - Job claiming and execution is fully lock-free
    - No blocking syscalls in the audio processing path (barrier + jobs)

    CPU usage characteristics:
    - Idle: Workers sleep → minimal CPU usage
    - Active: Brief spin on wake + adaptive spinning in barrier
    - Barrier: 8→16→32→64→128→256→512→1024→2048→4096→8192→16384 CPU_PAUSE, then yield forever
    - Total pauses: 32,760 (~10-50 microseconds on modern CPUs, very low power)

    Usage pattern:
    1. Get singleton instance: AudioThreadPool::getInstance()
    2. Configure: pool.configure(samplesPerBlock, sampleRate) during prepareToPlay()
    3. Submit jobs: prepareJobs(barrier), addJob(...), kickWorkers()
    4. Workers wake up (brief spin + sleep), execute jobs (lock-free), barrier (spinning)
    5. Main thread arrives at barrier (spinning) for completion
*/
class AudioThreadPool
{
public:
    //==============================================================================
    JUCE_DECLARE_SINGLETON_SINGLETHREADED_MINIMAL_INLINE(AudioThreadPool)

    void initialize(int numWorkers = 0, int priority = 8)
    {
        std::lock_guard<std::mutex> lock(poolMutex);

        if (isInitialized)
            return; // Already initialized

        // Auto-detect optimal worker count if not specified
        if (numWorkers <= 0)
        {
            const int physicalCores = juce::SystemStats::getNumPhysicalCpus();
            // Reserve 2 cores: 1 for main audio thread, 1 for system/GUI
            numWorkers = juce::jmax(1, physicalCores - 2);
        }

        // Create worker threads
        workerThreads.reserve(numWorkers);
        int realtimeThreadCount = 0;

        for (int i = 0; i < numWorkers; ++i)
        {
            auto worker = std::make_unique<WorkerThread>(
                "AudioThreadPool_" + juce::String(i),
                jobQueue,
                &currentBarrier,
                sharedWakeMutex,
                sharedWakeCV
            );

            // Try realtime scheduling first (priority 8 on 0-10 scale)
            // On Linux: requires CAP_SYS_NICE or root, uses SCHED_RR
            // On macOS/Windows: works without special privileges
            juce::Thread::RealtimeOptions rtOptions;
            rtOptions = rtOptions.withPriority(8);

            if (worker->startRealtimeThread(rtOptions))
            {
                ++realtimeThreadCount;
            }
            else
            {
                // Fallback: regular thread with highest priority
                // On Linux: uses SCHED_OTHER (no special privileges needed)
                worker->startThread();
                worker->setThreadPriority(juce::Thread::Priority::highest);
            }

            workerThreads.push_back(std::move(worker));
        }

        isInitialized = true;

        DBG("AudioThreadPool: Initialized with "
            << numWorkers
            << " worker threads ("
            << realtimeThreadCount
            << " realtime, "
            << (numWorkers - realtimeThreadCount)
            << " regular priority, CPU cores: "
            << juce::SystemStats::getNumPhysicalCpus()
            << ")");
    }

    /** Shutdown the thread pool and stop all workers */
    void shutdown()
    {
        std::lock_guard<std::mutex> lock(poolMutex);

        if (!isInitialized)
            return;

        DBG("AudioThreadPool: Shutting down " << workerThreads.size() << " worker threads...");

        // Worker destructors will handle shutdown cleanly
        workerThreads.clear();
        isInitialized = false;

        DBG("AudioThreadPool: Shutdown complete");
    }

    /** Check if pool is initialized */
    bool isReady() const
    {
        return isInitialized;
    }

    /** Get number of worker threads */
    int getNumWorkers() const
    {
        return static_cast<int>(workerThreads.size());
    }

    /** Configure adaptive backoff for thread barriers and workers based on buffer size and sample rate.
        Call this during prepareToPlay() to calibrate spin locks.
        Thread-safe: Can be called multiple times, benchmark happens only once globally.

        @param samplesPerBlock Buffer size in samples
        @param sampleRate Sample rate in Hz
    */
    void configure(int samplesPerBlock, double sampleRate)
    {
        // Store configuration
        currentSamplesPerBlock_ = samplesPerBlock;
        currentSampleRate_ = sampleRate;

        // Configure worker thread spin locks
        for (auto& worker : workerThreads)
            if (worker != nullptr)
                worker->configure(samplesPerBlock, sampleRate);
    }

    //==============================================================================
    /** Prepare jobs for parallel execution
        @param barrier Thread barrier for synchronization (workers + caller should wait on this)
        @return Number of jobs submitted

        Usage:
            auto barrier = ThreadBarrier::make(numJobs + 1); // +1 for caller thread
            barrier->configure(samplesPerBlock, sampleRate); // Configure before use
            pool.prepareJobs(std::move(barrier));
            pool.addJob(myFunction, &myContext);
            pool.kickWorkers();
            barrier->arriveAndWait(); // Wait for completion
    */
    void prepareJobs(ThreadBarrier::Ptr barrier)
    {
        // Thread pool supports two usage patterns:
        // 1. Synchronous (with barrier): prepareJobs(barrier) → addJobs → kickWorkers → arriveAndWait
        //    - Workers process jobs, then arrive at barrier
        //    - Caller must arrive at barrier before calling prepareJobs again
        // 2. Fire-and-forget (no barrier): prepareJobs(nullptr) → addJobs → kickWorkers → return
        //    - Workers process jobs, skip barrier arrival (barrier == nullptr)
        //    - Safe to call prepareJobs again even if previous jobs still executing
        //    - Worker's barrier identity check prevents arriving at wrong barrier

        // Safety check: Warn if jobs still unclaimed (possible race condition)
        const int remaining = jobQueue.getRemainingJobs();
        if (remaining > 0)
        {
            // Jobs still in flight - usually OK for fire-and-forget, but may indicate timing issue
            DBG("INFO: prepareJobs() called while " << remaining << " jobs still unclaimed.");
        }

        currentBarrier = std::move(barrier);
        jobQueue.reset();
    }

    void addJob(void (*execute)(void*), void* userData)
    {
        jobQueue.addJob(execute, userData);
    }

    void kickWorkers()
    {
        // Set all work flags first
        for (auto& worker : workerThreads)
            if (worker != nullptr)
                worker->notify();

        // Wake all workers at once
        {
            std::lock_guard<std::mutex> lock(sharedWakeMutex);
            sharedWakeCV.notify_all();
        }
    }

    bool tryStealAndExecuteJob()
    {
        RealtimeJobQueue::Job* job = nullptr;
        if (jobQueue.tryClaimJob(job) && job != nullptr && job->isValid())
        {
            job->run();
            job->completed.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    ThreadBarrier::Ptr getCurrentBarrier() const
    {
        return currentBarrier;
    }

private:
    class WorkerThread : public juce::Thread
    {
    public:
        WorkerThread(
            const juce::String& name,
            RealtimeJobQueue& queue,
            ThreadBarrier::Ptr* barrierPtr,
            std::mutex& sharedMutex,
            std::condition_variable& sharedCV
        )
            : juce::Thread(name)
            , jobQueue(queue)
            , currentBarrier(barrierPtr)
            , workFlag(false)
            , wakeMutex(sharedMutex)
            , cv(sharedCV)
        {
        }

        ~WorkerThread() override
        {
            signalThreadShouldExit();

            // Wake up the thread by notifying the condition variable
            {
                std::lock_guard<std::mutex> lock(wakeMutex);
                workFlag.store(true, std::memory_order_release);
                cv.notify_all();
            }

            stopThread(2000);
        }

        void notify()
        {
            workFlag.store(true, std::memory_order_release);
            // Note: cv.notify_all() is called by kickWorkers() after all notify() calls
        }

        void setThreadPriority(juce::Thread::Priority priority)
        {
            setPriority(priority);
        }

        void configure(int samplesPerBlock, double sampleRate)
        {
            (void)samplesPerBlock;
            (void)sampleRate;
        }

    private:
        void run() override
        {
            while (!threadShouldExit())
            {
                // Wait for work using condition variable - efficient blocking with fast wake
                {
                    std::unique_lock<std::mutex> lock(wakeMutex);
                    cv.wait(lock, [this] { return workFlag.load(std::memory_order_acquire) || threadShouldExit(); });
                }

                if (threadShouldExit())
                    return;

                workFlag.store(false, std::memory_order_release);

                // Capture barrier at start - must not arrive if barrier changes mid-execution
                auto barrier = currentBarrier ? *currentBarrier : nullptr;

                RealtimeJobQueue::Job* job = nullptr;
                while (jobQueue.tryClaimJob(job))
                {
                    if (job && job->isValid())
                    {
                        job->run();
                        job->completed.store(true, std::memory_order_release);
                    }

                    if (threadShouldExit())
                        return;
                }

                // Only arrive at barrier if it hasn't changed since we started
                // (prevents arriving at wrong barrier if prepareJobs() was called during job execution)
                if (barrier != nullptr)
                {
                    auto currentBarrierSnapshot = currentBarrier ? *currentBarrier : nullptr;
                    if (barrier == currentBarrierSnapshot)
                        barrier->arriveAndWait();
                    // else: Barrier changed - someone else called prepareJobs(), don't arrive
                }
            }
        }

        RealtimeJobQueue& jobQueue;
        ThreadBarrier::Ptr* currentBarrier;
        std::atomic<bool> workFlag;
        std::mutex& wakeMutex;
        std::condition_variable& cv;
    };

    //==============================================================================
    AudioThreadPool()
        : isInitialized(false)
    {
    }

    ~AudioThreadPool()
    {
        shutdown();
    }

    //==============================================================================
    std::vector<std::unique_ptr<WorkerThread>> workerThreads;
    RealtimeJobQueue jobQueue;
    ThreadBarrier::Ptr currentBarrier;
    std::mutex poolMutex;
    std::atomic<bool> isInitialized{false};
    int currentSamplesPerBlock_{512};
    double currentSampleRate_{48000.0};

    // Shared condition variable for all workers
    std::mutex sharedWakeMutex;
    std::condition_variable sharedWakeCV;

    AudioThreadPool(const AudioThreadPool&) = delete;
    AudioThreadPool& operator=(const AudioThreadPool&) = delete;
    AudioThreadPool(AudioThreadPool&&) = delete;
    AudioThreadPool& operator=(AudioThreadPool&&) = delete;
};

} // namespace atk
