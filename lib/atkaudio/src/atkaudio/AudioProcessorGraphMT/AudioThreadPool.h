// Copyright (c) 2024 atkAudio

#pragma once

#include "../CpuInfo.h"
#include "../RealtimeThread.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace atk
{

//==============================================================================
/** Thread barrier type alias using C++20 std::barrier */
using ThreadBarrier = std::shared_ptr<std::barrier<>>;

inline ThreadBarrier makeBarrier(int numThreadsToSynchronise)
{
    return std::make_shared<std::barrier<>>(numThreadsToSynchronise);
}

//==============================================================================
/**
    Job context for audio processing jobs.
    Fixed-size struct to avoid std::function allocations.
*/
struct AudioJobContext
{
    void* userData = nullptr;
    void (*execute)(void*) = nullptr;
    std::atomic<bool> completed{false};

    AudioJobContext()
        : userData(nullptr)
        , execute(nullptr)
        , completed(false)
    {
    }

    AudioJobContext(AudioJobContext&& other) noexcept
        : userData(other.userData)
        , execute(other.execute)
        , completed(other.completed.load(std::memory_order_acquire))
    {
        other.userData = nullptr;
        other.execute = nullptr;
    }

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
        jobs.resize(8192);
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
        while (true)
        {
            int currentTail = tail.load(std::memory_order_acquire);
            const int currentHead = head.load(std::memory_order_acquire);

            if (currentTail >= currentHead)
                return false;

            if (tail.compare_exchange_weak(
                    currentTail,
                    currentTail + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                ))
            {
                const size_t index = currentTail % jobs.size();
                outJob = &jobs[index];
                return outJob->isValid();
            }
        }
    }

    int getRemainingJobs() const
    {
        const int h = head.load(std::memory_order_acquire);
        const int t = tail.load(std::memory_order_acquire);
        return (std::max)(0, h - t);
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
    std::atomic<int> head;
    std::atomic<int> tail;

    RealtimeJobQueue(const RealtimeJobQueue&) = delete;
    RealtimeJobQueue& operator=(const RealtimeJobQueue&) = delete;
};

//==============================================================================
/**
    Global audio thread pool singleton - REALTIME SAFE AUDIO PATH.

    This thread pool is designed for realtime audio processing with the following features:
    - Singleton instance shared across all audio graph processors
    - Persistent worker threads to avoid creation/destruction overhead
    - Lock-free job queue for realtime-safe job distribution (atomic fetch_add)
    - Barrier synchronization using mutex + condition_variable
    - High-priority threads with platform-specific realtime scheduling

    Thread safety:
    - getInstance()/deleteInstance(): Call only during program load/unload (single-threaded)
    - initialize()/shutdown(): Thread-safe via mutex
    - prepareJobs(): Call only from main audio thread (before kickWorkers)
    - addJob(): Call only from main audio thread (between prepareJobs and kickWorkers)
    - kickWorkers(): Call only from main audio thread
    - tryStealAndExecuteJob(): Safe to call from any thread

    Job queue safety:
    - Queue is reset via prepareJobs() before each batch
    - Maximum 1024 jobs per batch (queue size)
    - Jobs are processed before next prepareJobs() call

    Usage pattern:
       prepareJobs(barrier);          // Main thread: setup barrier, reset queue
       addJob(...);                   // Main thread: add jobs (max 1024)
       kickWorkers();                 // Main thread: wake workers
       barrier->arrive_and_wait();    // Wait for completion
*/
class AudioThreadPool
{
public:
    /** Get singleton instance.
        @note Not thread-safe. Call only during single-threaded program initialization. */
    static AudioThreadPool* getInstance()
    {
        if (!instance)
            instance = new AudioThreadPool();
        return instance;
    }

    static void deleteInstance()
    {
        delete instance;
        instance = nullptr;
    }

    void initialize(int numWorkers = 0, int /*priority*/ = 8)
    {
        std::lock_guard<std::mutex> lock(poolMutex);

        if (isInitialized)
            return;

        if (numWorkers <= 0)
        {
            const int physicalCores = getNumPhysicalCpus();
            numWorkers = (std::max)(1, physicalCores - 2);
        }

        workerThreads.reserve(numWorkers);

        for (int i = 0; i < numWorkers; ++i)
            workerThreads.push_back(std::make_unique<WorkerThread>(*this));

        isInitialized = true;

#ifndef NDEBUG
        printf(
            "AudioThreadPool initialized with %d worker threads (CPU cores: %d)\n",
            numWorkers,
            getNumPhysicalCpus()
        );
#endif
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(poolMutex);

        if (!isInitialized)
            return;

        workerThreads.clear();
        isInitialized = false;
    }

    bool isReady() const
    {
        return isInitialized;
    }

    int getNumWorkers() const
    {
        return static_cast<int>(workerThreads.size());
    }

    void configure(int /*samplesPerBlock*/, double /*sampleRate*/)
    {
        // Reserved for future use (e.g., adaptive spin lock tuning)
    }

    void prepareJobs(ThreadBarrier newBarrier)
    {
        barrier = std::move(newBarrier);
        jobQueue.reset();
    }

    void addJob(void (*execute)(void*), void* userData)
    {
        jobQueue.addJob(execute, userData);
    }

    void kickWorkers()
    {
        // Set all ready flags first
        for (auto& worker : workerThreads)
            if (worker != nullptr)
                worker->setReady();

        // Wake all workers at once
        {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
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

    ThreadBarrier getBarrier() const
    {
        return barrier;
    }

    bool isCalledFromWorkerThread() const
    {
        auto currentId = std::this_thread::get_id();
        for (const auto& worker : workerThreads)
            if (worker && worker->getThreadId() == currentId)
                return true;
        return false;
    }

    RealtimeJobQueue& getJobQueue()
    {
        return jobQueue;
    }

    std::mutex& getMutex()
    {
        return mutex;
    }

    std::condition_variable& getCV()
    {
        return cv;
    }

private:
    class WorkerThread
    {
    public:
        explicit WorkerThread(AudioThreadPool& poolRef)
            : pool(poolRef)
            , ready(false)
            , shouldExit(false)
            , thread(&WorkerThread::run, this)
        {
            trySetRealtimePriority(thread);
        }

        ~WorkerThread()
        {
            shouldExit.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(pool.getMutex());
                ready.store(true, std::memory_order_release);
            }
            pool.getCV().notify_all();
            if (thread.joinable())
                thread.join();
        }

        void setReady()
        {
            ready.store(true, std::memory_order_release);
        }

        std::thread::id getThreadId() const
        {
            return thread.get_id();
        }

    private:
        void run()
        {
            while (!shouldExit.load(std::memory_order_acquire))
            {
                // Wait for work signal
                {
                    std::unique_lock<std::mutex> lock(pool.getMutex());
                    pool.getCV().wait(
                        lock,
                        [this]
                        { return ready.load(std::memory_order_acquire) || shouldExit.load(std::memory_order_acquire); }
                    );
                    ready.store(false, std::memory_order_release);
                }

                if (shouldExit.load(std::memory_order_acquire))
                    return;

                // Process all available jobs
                RealtimeJobQueue::Job* job = nullptr;
                while (pool.getJobQueue().tryClaimJob(job))
                {
                    if (job && job->isValid())
                    {
                        job->run();
                        job->completed.store(true, std::memory_order_release);
                    }

                    if (shouldExit.load(std::memory_order_acquire))
                        return;
                }

                // Synchronize at barrier
                if (auto syncBarrier = pool.getBarrier())
                    syncBarrier->arrive_and_wait();
            }
        }

        AudioThreadPool& pool;
        std::atomic<bool> ready;
        std::atomic<bool> shouldExit;
        std::thread thread;
    };

    AudioThreadPool()
        : isInitialized(false)
    {
    }

public:
    ~AudioThreadPool()
    {
        shutdown();
    }

private:
    inline static AudioThreadPool* instance = nullptr;

    std::vector<std::unique_ptr<WorkerThread>> workerThreads;
    RealtimeJobQueue jobQueue;
    ThreadBarrier barrier;
    std::mutex poolMutex;
    std::atomic<bool> isInitialized{false};

    // Shared condition variable for all workers
    std::mutex mutex;
    std::condition_variable cv;

    AudioThreadPool(const AudioThreadPool&) = delete;
    AudioThreadPool& operator=(const AudioThreadPool&) = delete;
};

} // namespace atk