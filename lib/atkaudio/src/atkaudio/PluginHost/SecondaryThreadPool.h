// Copyright (c) 2024 atkAudio
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace atk
{

/**
    Dedicated thread pool for PluginHost fire-and-forget jobs.
    Completely separate from AudioThreadPool used by PluginHost2.
    Singleton pattern with lock-free circular job queue for realtime safety.
*/
class SecondaryThreadPool
{
public:
    JUCE_DECLARE_SINGLETON_SINGLETHREADED_MINIMAL_INLINE(SecondaryThreadPool)

    struct Job
    {
        void (*execute)(void*) = nullptr;
        void* userData = nullptr;

        Job() = default;

        Job(void (*exec)(void*), void* data)
            : execute(exec)
            , userData(data)
        {
        }

        bool isValid() const
        {
            return execute != nullptr && userData != nullptr;
        }

        void run()
        {
            if (execute && userData)
                execute(userData);
        }
    };

    void initialize(int numThreads = 0, int maxJobs = 1024)
    {
        if (isInitialized.load(std::memory_order_acquire))
            return;

        std::lock_guard<std::mutex> lock(poolMutex);

        if (isInitialized.load(std::memory_order_acquire))
            return;

        if (numThreads <= 0)
            numThreads = juce::jmax(1, juce::SystemStats::getNumCpus() - 1);

        // Pre-allocate circular buffer for lock-free job queue
        jobs.resize(maxJobs);
        head.store(0, std::memory_order_release);
        tail.store(0, std::memory_order_release);

        workerThreads.clear();
        workFlags.clear();
        int realtimeThreadCount = 0;

        for (int i = 0; i < numThreads; ++i)
        {
            // Create work flag for this worker
            workFlags.push_back(std::make_unique<std::atomic<bool>>(false));

            auto worker = std::make_unique<WorkerThread>(
                "SecondaryPool_" + juce::String(i),
                jobs,
                head,
                tail,
                sharedWakeMutex,
                sharedWakeCV,
                *workFlags.back()
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

        isInitialized.store(true, std::memory_order_release);

        DBG("SecondaryThreadPool: Initialized with "
            << numThreads
            << " worker threads ("
            << realtimeThreadCount
            << " realtime, "
            << (numThreads - realtimeThreadCount)
            << " regular priority, CPU cores: "
            << juce::SystemStats::getNumPhysicalCpus()
            << ")");
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(poolMutex);

        if (!isInitialized.load(std::memory_order_acquire))
            return;

        DBG("SecondaryThreadPool: Shutting down " << workerThreads.size() << " worker threads...");

        // Worker destructors will handle shutdown cleanly
        workerThreads.clear();
        isInitialized.store(false, std::memory_order_release);

        DBG("SecondaryThreadPool: Shutdown complete");
    }

    bool isReady() const
    {
        return isInitialized.load(std::memory_order_acquire);
    }

    // Realtime-safe: lock-free circular buffer with atomic head/tail
    void addJob(void (*execute)(void*), void* userData)
    {
        const size_t index = head.fetch_add(1, std::memory_order_acq_rel) % jobs.size();
        jobs[index].execute = execute;
        jobs[index].userData = userData;
    }

    // Realtime-safe: only sets atomic flags and wakes workers
    void kickWorkers()
    {
        // Set all work flags first (lock-free)
        for (auto& flag : workFlags)
            flag->store(true, std::memory_order_release);

        // Wake workers (syscall, but unavoidable for efficient sleep/wake)
        {
            std::lock_guard<std::mutex> lock(sharedWakeMutex);
            sharedWakeCV.notify_all();
        }
    }

private:
    class WorkerThread : public juce::Thread
    {
    public:
        WorkerThread(
            const juce::String& name,
            std::vector<Job>& jobsBuffer,
            std::atomic<int>& headPtr,
            std::atomic<int>& tailPtr,
            std::mutex& sharedMutex,
            std::condition_variable& sharedCV,
            std::atomic<bool>& workFlag
        )
            : juce::Thread(name)
            , jobs(jobsBuffer)
            , head(headPtr)
            , tail(tailPtr)
            , wakeMutex(sharedMutex)
            , wakeCV(sharedCV)
            , workFlag(workFlag)
        {
        }

        ~WorkerThread() override
        {
            signalThreadShouldExit();
            wakeCV.notify_all();
            stopThread(2000);
        }

        void setThreadPriority(juce::Thread::Priority priority)
        {
            setPriority(priority);
        }

    private:
        void run() override
        {
            while (!threadShouldExit())
            {
                // Wait for work using condition variable
                {
                    std::unique_lock<std::mutex> lock(wakeMutex);
                    wakeCV.wait(
                        lock,
                        [this] { return workFlag.load(std::memory_order_acquire) || threadShouldExit(); }
                    );
                }

                if (threadShouldExit())
                    return;

                workFlag.store(false, std::memory_order_release);

                // Process jobs using lock-free circular buffer (CAS-based claiming)
                while (true)
                {
                    int currentTail = tail.load(std::memory_order_acquire);
                    const int currentHead = head.load(std::memory_order_acquire);

                    // No more jobs available
                    if (currentTail >= currentHead)
                        break;

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
                        Job& job = jobs[index];

                        if (job.isValid())
                            job.run();
                    }
                    // CAS failed - another worker claimed it, retry
                }
            }
        }

        std::vector<Job>& jobs;
        std::atomic<int>& head;
        std::atomic<int>& tail;
        std::mutex& wakeMutex;
        std::condition_variable& wakeCV;
        std::atomic<bool>& workFlag;
    };

    //==============================================================================
    SecondaryThreadPool()
        : isInitialized(false)
        , head(0)
        , tail(0)
    {
    }

    ~SecondaryThreadPool()
    {
        shutdown();
        clearSingletonInstance();
    }

    //==============================================================================
    std::vector<std::unique_ptr<WorkerThread>> workerThreads;
    std::vector<Job> jobs;                                     // Pre-allocated circular buffer
    std::atomic<int> head;                                     // Producer writes here
    std::atomic<int> tail;                                     // Consumers read from here
    std::vector<std::unique_ptr<std::atomic<bool>>> workFlags; // Per-worker wake flags
    std::mutex poolMutex;
    std::mutex sharedWakeMutex;
    std::condition_variable sharedWakeCV;
    std::atomic<bool> isInitialized{false};

    SecondaryThreadPool(const SecondaryThreadPool&) = delete;
    SecondaryThreadPool& operator=(const SecondaryThreadPool&) = delete;
    SecondaryThreadPool(SecondaryThreadPool&&) = delete;
    SecondaryThreadPool& operator=(SecondaryThreadPool&&) = delete;
};

} // namespace atk
