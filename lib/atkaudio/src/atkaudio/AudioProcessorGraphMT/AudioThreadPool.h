#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

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
        std::unique_lock<std::mutex> lk{mutex};

        const int myGeneration = generation;
        [[maybe_unused]] const auto c = ++blockCount;

        // You've tried to synchronise too many threads!!
        jassert(c <= threadCount);

        if (blockCount == threadCount)
        {
            blockCount = 0;
            ++generation;
            cv.notify_all();
            return;
        }

        cv.wait(lk, [this, myGeneration] { return generation != myGeneration; });
    }

private:
    explicit ThreadBarrier(int numThreadsToSynchronise)
        : threadCount(numThreadsToSynchronise)
        , blockCount(0)
        , generation(0)
    {
    }

    std::mutex mutex;
    std::condition_variable cv;
    int blockCount;
    int generation;
    const int threadCount;

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
        : head(0)
        , tail(0)
    {
        jobs.resize(1024);
    }

    void reset()
    {
        head.store(0, std::memory_order_release);
        tail.store(0, std::memory_order_release);
    }

    size_t addJob(void (*execute)(void*), void* userData)
    {
        const size_t index = head.fetch_add(1, std::memory_order_acq_rel) % jobs.size();

        jobs[index].execute = execute;
        jobs[index].userData = userData;
        jobs[index].completed.store(false, std::memory_order_release);

        return index;
    }

    bool tryClaimJob(Job*& outJob)
    {
        // Try to claim a job by incrementing tail, but only if tail < head
        while (true)
        {
            int currentTail = tail.load(std::memory_order_acquire);
            const int currentHead = head.load(std::memory_order_acquire);

            // No more jobs available
            if (currentTail >= currentHead)
                return false;

            // Try to claim this job with CAS
            if (tail.compare_exchange_weak(
                    currentTail,
                    currentTail + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                ))
            {
                // Successfully claimed job at currentTail
                const size_t index = currentTail % jobs.size();
                outJob = &jobs[index];
                return outJob->isValid();
            }
            // CAS failed - another worker claimed it, retry
        }
    }

    int getRemainingJobs() const
    {
        const int h = head.load(std::memory_order_acquire);
        const int t = tail.load(std::memory_order_acquire);
        return juce::jmax(0, h - t);
    }

    int getTotalJobs() const
    {
        return head.load(std::memory_order_acquire);
    }

    int getTail() const
    {
        return tail.load(std::memory_order_acquire);
    }

private:
    std::vector<Job> jobs;
    std::atomic<int> head; // Producer writes here (increments after adding)
    std::atomic<int> tail; // Consumer reads from here (increments after claiming)

    JUCE_DECLARE_NON_COPYABLE(RealtimeJobQueue)
};

//==============================================================================
/**
    Global audio thread pool singleton - REALTIME SAFE AUDIO PATH.

    This thread pool is designed for realtime audio processing with the following features:
    - Singleton instance shared across all audio graph processors
    - Persistent worker threads to avoid creation/destruction overhead
    - Lock-free job queue for realtime-safe job distribution (atomic fetch_add)
    - Barrier synchronization using mutex + condition_variable
    - High-priority threads (not realtime scheduled, for stability)
    - Uses DeletedAtShutdown for proper cleanup order

    Job Queue Architecture:
    - Barrier-synchronized jobs for parallel audio graph processing
      * Workers process jobs then arrive at barrier
      * Main thread waits at barrier for completion
      * Critical audio path processing

    Hybrid approach for optimal CPU usage:
    - WorkerThread idle wait (between callbacks): condition_variable (low CPU)
    - ThreadBarrier (during audio processing): mutex + condition_variable
    - RealtimeJobQueue: atomic fetch_add for lock-free job claiming (realtime-safe)
    - Job processing: fully lock-free and realtime-safe

    Why hybrid?
    - Workers sleep between audio callbacks (50-100Hz rate) → low CPU when idle
    - Barrier synchronization during audio processing uses efficient OS primitives
    - Job claiming and execution is fully lock-free
    - No busy-waiting or excessive spinning

    CPU usage characteristics:
    - Idle: Workers sleep → minimal CPU usage
    - Active: Efficient wake via condition_variable
    - Barrier: Mutex + condition_variable (OS-level blocking, no spinning)

    Usage pattern:
       prepareJobs(barrier);          // Setup barrier for job queue
       addJob(...);                   // Audio graph processing
       kickWorkers();                 // Wake workers
       barrier->arriveAndWait();      // Wait for completion
*/
class AudioThreadPool
{
public:
    //==============================================================================
    JUCE_DECLARE_SINGLETON_SINGLETHREADED_MINIMAL_INLINE(AudioThreadPool)

    void initialize(int numWorkers = 0, int priority = 8)
    {
        (void)priority; // Unused - reserved for future use
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
                sharedWakeCV,
                this
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

    bool isCalledFromWorkerThread() const
    {
        auto currentThread = juce::Thread::getCurrentThread();
        for (const auto& worker : workerThreads)
            if (worker.get() == currentThread)
                return true;
        return false;
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
            std::condition_variable& sharedCV,
            AudioThreadPool* parentPool
        )
            : juce::Thread(name)
            , jobQueue(queue)
            , currentBarrier(barrierPtr)
            , workFlag(false)
            , wakeMutex(sharedMutex)
            , cv(sharedCV)
            , pool(parentPool)
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

                // Capture barrier at start - prevents race with next cycle
                auto barrier = currentBarrier ? *currentBarrier : nullptr;

                // Process jobs
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

                // Arrive at barrier (if present and still valid)
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
        AudioThreadPool* pool;
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
