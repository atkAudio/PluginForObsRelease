// Copyright (c) 2024 atkAudio

#pragma once

#include "../CpuInfo.h"
#include "../RealtimeThread.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>

namespace atk
{

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
    Thread barrier for real-time audio thread synchronization.
*/
class ThreadBarrier
{
public:
    using Ptr = std::shared_ptr<ThreadBarrier>;

    static Ptr make(int numThreadsToSynchronise)
    {
        return std::make_shared<ThreadBarrier>(numThreadsToSynchronise);
    }

    explicit ThreadBarrier(int numThreadsToSynchronise)
        : threadCount(numThreadsToSynchronise)
        , blockCount(0)
        , generation(0)
    {
    }

    void configure(int /*samplesPerBlock*/, double /*sampleRate*/)
    {
    }

    void arriveAndWait()
    {
        std::unique_lock<std::mutex> lk{mutex};

        const int myGeneration = generation;
        [[maybe_unused]] const int c = ++blockCount;

        assert(c <= threadCount);

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
    std::mutex mutex;
    std::condition_variable cv;
    int blockCount;
    int generation;
    const int threadCount;

    ThreadBarrier(const ThreadBarrier&) = delete;
    ThreadBarrier& operator=(const ThreadBarrier&) = delete;
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
       barrier->arriveAndWait();      // Wait for completion
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
        {
            workerThreads.push_back(
                std::make_unique<WorkerThread>(jobQueue, &currentBarrier, sharedWakeMutex, sharedWakeCV)
            );
        }

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
        for (auto& worker : workerThreads)
            if (worker != nullptr)
                worker->notify();

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
        auto currentId = std::this_thread::get_id();
        for (const auto& worker : workerThreads)
            if (worker && worker->getThreadId() == currentId)
                return true;
        return false;
    }

private:
    class WorkerThread
    {
    public:
        WorkerThread(
            RealtimeJobQueue& queue,
            ThreadBarrier::Ptr* barrierPtr,
            std::mutex& sharedMutex,
            std::condition_variable& sharedCV
        )
            : jobQueue(queue)
            , currentBarrier(barrierPtr)
            , wakeMutex(sharedMutex)
            , cv(sharedCV)
            , workFlag(false)
            , shouldExit(false)
            , thread(&WorkerThread::run, this)
        {
            trySetRealtimePriority(thread);
        }

        ~WorkerThread()
        {
            shouldExit.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(wakeMutex);
                workFlag.store(true, std::memory_order_release);
                cv.notify_all();
            }
            if (thread.joinable())
                thread.join();
        }

        void notify()
        {
            workFlag.store(true, std::memory_order_release);
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
                {
                    std::unique_lock<std::mutex> lock(wakeMutex);
                    cv.wait(
                        lock,
                        [this]
                        {
                            return workFlag.load(std::memory_order_acquire)
                                || shouldExit.load(std::memory_order_acquire);
                        }
                    );
                }

                if (shouldExit.load(std::memory_order_acquire))
                    return;

                workFlag.store(false, std::memory_order_release);

                auto barrier = currentBarrier ? *currentBarrier : nullptr;

                RealtimeJobQueue::Job* job = nullptr;
                while (jobQueue.tryClaimJob(job))
                {
                    if (job && job->isValid())
                    {
                        job->run();
                        job->completed.store(true, std::memory_order_release);
                    }

                    if (shouldExit.load(std::memory_order_acquire))
                        return;
                }

                if (barrier != nullptr)
                {
                    auto currentBarrierSnapshot = currentBarrier ? *currentBarrier : nullptr;
                    if (barrier == currentBarrierSnapshot)
                        barrier->arriveAndWait();
                }
            }
        }

        RealtimeJobQueue& jobQueue;
        ThreadBarrier::Ptr* currentBarrier;
        std::mutex& wakeMutex;
        std::condition_variable& cv;
        std::atomic<bool> workFlag;
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
    ThreadBarrier::Ptr currentBarrier;
    std::mutex poolMutex;
    std::atomic<bool> isInitialized{false};

    std::mutex sharedWakeMutex;
    std::condition_variable sharedWakeCV;

    AudioThreadPool(const AudioThreadPool&) = delete;
    AudioThreadPool& operator=(const AudioThreadPool&) = delete;
};

} // namespace atk