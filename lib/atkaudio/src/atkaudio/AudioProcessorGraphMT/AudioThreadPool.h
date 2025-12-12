// Copyright (c) 2024 atkAudio

#pragma once

#include "../CpuInfo.h"
#include "../RealtimeThread.h"
#include "DependencyTaskGraph.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace atk
{

//==============================================================================
/**
    Lock-free MPMC task queue for parallel audio processing.
*/
class RealtimeTaskQueue
{
public:
    RealtimeTaskQueue()
        : head(0)
        , tail(0)
    {
        static_assert((kCapacity & (kCapacity - 1)) == 0, "Capacity must be power of 2");
        for (size_t i = 0; i < kCapacity; ++i)
            slots[i].sequence.store(i, std::memory_order_relaxed);
    }

    void reset()
    {
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < kCapacity; ++i)
            slots[i].sequence.store(i, std::memory_order_relaxed);
    }

    struct Task
    {
        void* userData = nullptr;
        void (*execute)(void*) = nullptr;
    };

    bool tryPush(void (*execute)(void*), void* userData)
    {
        size_t pos = head.load(std::memory_order_relaxed);
        for (;;)
        {
            Slot& slot = slots[pos & (kCapacity - 1)];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0)
            {
                if (head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    slot.task.execute = execute;
                    slot.task.userData = userData;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
                return false; // Queue full
            else
                pos = head.load(std::memory_order_relaxed);
        }
    }

    bool tryPop(Task& outTask)
    {
        size_t pos = tail.load(std::memory_order_relaxed);
        for (;;)
        {
            Slot& slot = slots[pos & (kCapacity - 1)];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0)
            {
                if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    outTask = slot.task;
                    slot.sequence.store(pos + kCapacity, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
                return false; // Queue empty
            else
                pos = tail.load(std::memory_order_relaxed);
        }
    }

    bool isEmpty() const
    {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
    }

private:
    static constexpr size_t kCapacity = 8192;

    struct Slot
    {
        std::atomic<size_t> sequence;
        Task task;
    };

    alignas(64) std::atomic<size_t> head;
    alignas(64) std::atomic<size_t> tail;
    alignas(64) Slot slots[kCapacity];

    RealtimeTaskQueue(const RealtimeTaskQueue&) = delete;
    RealtimeTaskQueue& operator=(const RealtimeTaskQueue&) = delete;
};

//==============================================================================
/**
    Global realtime thread pool singleton.
    Workers pull tasks from queue and execute them. That's it.
*/
class AudioThreadPool
{
public:
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

    void initialize(int numWorkers = 0)
    {
        std::lock_guard<std::mutex> lock(poolMutex);

        if (initialized.load(std::memory_order_acquire))
            return;

        if (numWorkers <= 0)
            numWorkers = (std::max)(1, getNumPhysicalCpus() - 2);

        // Get physical core mapping - pin workers to physical cores
        // Reserve first 2 physical cores for main thread and OS (if available)
        auto physicalCores = getPhysicalCoreMapping();
        const int numPhysical = static_cast<int>(physicalCores.size());

        DBG("[AudioThreadPool] Initializing with "
            << numWorkers
            << " workers, "
            << numPhysical
            << " physical cores detected");

        for (int i = 0; i < numWorkers; ++i)
        {
            int coreId = -1;
            if (numPhysical > 2)
            {
                int physicalIndex = 2 + (i % (numPhysical - 2));
                coreId = physicalCores[physicalIndex];
            }
            else if (numPhysical > 0)
            {
                coreId = physicalCores[i % numPhysical];
            }
            workers.push_back(std::make_unique<Worker>(*this, coreId));
        }

        DBG("[AudioThreadPool] All workers started");

        initialized.store(true, std::memory_order_release);
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(poolMutex);

        if (!initialized.load(std::memory_order_acquire))
            return;

        workers.clear();
        initialized.store(false, std::memory_order_release);
    }

    bool isReady() const
    {
        return initialized.load(std::memory_order_acquire);
    }

    int getNumWorkers() const
    {
        return static_cast<int>(workers.size());
    }

    /** Submit a task for execution. Returns true if queued successfully. */
    bool submitTask(void (*execute)(void*), void* userData)
    {
        if (!initialized.load(std::memory_order_acquire) || execute == nullptr)
            return false;

        if (!taskQueue.tryPush(execute, userData))
            return false;

        // Wake one worker
        cv.notify_one();
        return true;
    }

    /** Try to steal and execute one task (for caller participation). Returns true if executed. */
    bool tryExecuteTask()
    {
        RealtimeTaskQueue::Task task;
        if (taskQueue.tryPop(task) && task.execute != nullptr)
        {
            task.execute(task.userData);
            return true;
        }
        return false;
    }

    bool isCalledFromWorkerThread() const
    {
        auto currentId = std::this_thread::get_id();
        for (const auto& w : workers)
            if (w && w->getThreadId() == currentId)
                return true;
        return false;
    }

    void executeDependencyGraph(DependencyTaskGraph* graph)
    {
        if (!graph || graph->empty())
            return;

        const int numWorkerThreads = getNumWorkers();
        graph->setNumWorkers(numWorkerThreads);
        graph->prepare();

        for (int i = 0; i < numWorkerThreads; ++i)
        {
            workerContexts[i].graph = graph;
            workerContexts[i].workerId = i;
            submitTask(&executeGraphHelperWithAffinity, &workerContexts[i]);
        }

        graph->executeUntilDone();
    }

private:
    struct WorkerContext
    {
        DependencyTaskGraph* graph = nullptr;
        int workerId = -1;
    };

    // Pre-allocated to avoid allocation during audio callback
    WorkerContext workerContexts[32];

    static void executeGraphHelperWithAffinity(void* userData)
    {
        auto* ctx = static_cast<WorkerContext*>(userData);
        if (ctx && ctx->graph)
            ctx->graph->executeUntilDoneForWorker(ctx->workerId);
    }

    class Worker
    {
    public:
        explicit Worker(AudioThreadPool& p, int coreId = -1)
            : pool(p)
            , thread(&Worker::run, this)
        {
            trySetRealtimePriority(thread);

            if (coreId >= 0)
            {
                if (tryPinThreadToCore(thread, coreId))
                    DBG("[AudioThreadPool] Worker pinned to core " << coreId);
            }
        }

        ~Worker()
        {
            shouldExit.store(true, std::memory_order_release);
            pool.cv.notify_all();
            if (thread.joinable())
                thread.join();
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
                RealtimeTaskQueue::Task task;
                if (pool.taskQueue.tryPop(task) && task.execute != nullptr)
                {
                    task.execute(task.userData);
                }
                else
                {
                    // No tasks - wait for notification
                    std::unique_lock<std::mutex> lock(pool.mutex);
                    pool.cv.wait(
                        lock,
                        [this] { return shouldExit.load(std::memory_order_acquire) || !pool.taskQueue.isEmpty(); }
                    );
                }
            }
        }

        AudioThreadPool& pool;
        std::atomic<bool> shouldExit{false};
        std::thread thread;
    };

    AudioThreadPool() = default;

public:
    ~AudioThreadPool()
    {
        shutdown();
    }

private:
    inline static AudioThreadPool* instance = nullptr;

    std::vector<std::unique_ptr<Worker>> workers;
    RealtimeTaskQueue taskQueue;
    std::mutex poolMutex;
    std::atomic<bool> initialized{false};

    std::mutex mutex;
    std::condition_variable cv;

    AudioThreadPool(const AudioThreadPool&) = delete;
    AudioThreadPool& operator=(const AudioThreadPool&) = delete;
};

} // namespace atk
