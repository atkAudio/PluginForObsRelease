// Copyright (c) 2025 atkAudio

#pragma once

#include "../CpuInfo.h"
#include "../RealtimeThread.h"
#include "DependencyTaskGraph.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace atk
{

//==============================================================================
/**
    Lock-free MPMC task queue for fire-and-forget tasks.
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
                return false;
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
                return false;
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
    Realtime thread pool for parallel task execution.
    Supports both fire-and-forget tasks and dependency graph execution.
*/
class RealtimeThreadPool
{
public:
    static constexpr int kMaxWorkers = 32;

    static RealtimeThreadPool* getInstance()
    {
        if (!instance)
            instance = new RealtimeThreadPool();
        return instance;
    }

    static void deleteInstance()
    {
        delete instance;
        instance = nullptr;
    }

    void initialize(int numWorkers = 0)
    {
        if (initialized.load(std::memory_order_acquire))
            return;

        if (numWorkers <= 0)
            numWorkers = (std::max)(1, getNumPhysicalCpus() - 2);

        auto physicalCores = getPhysicalCoreMapping();
        const int numPhysical = static_cast<int>(physicalCores.size());

        DBG("[RealtimeThreadPool] Initializing with " << numWorkers << " workers");

        for (int i = 0; i < numWorkers; ++i)
        {
            int coreId = -1;
            if (numPhysical > 2)
                coreId = physicalCores[2 + (i % (numPhysical - 2))];
            else if (numPhysical > 0)
                coreId = physicalCores[i % numPhysical];

            workers.push_back(std::make_unique<Worker>(*this, i, coreId));
        }

        for (auto& w : workers)
            w->waitUntilStarted();

        initialized.store(true, std::memory_order_release);
    }

    void shutdown()
    {
        if (!initialized.load(std::memory_order_acquire))
            return;

        initialized.store(false, std::memory_order_release);

        // Workers handle their own cleanup in destructor
        workers.clear();

        currentGraph.store(nullptr, std::memory_order_release);
    }

    bool isReady() const
    {
        return initialized.load(std::memory_order_acquire);
    }

    int getNumWorkers() const
    {
        return static_cast<int>(workers.size());
    }

    // Submit a fire-and-forget task (wakes all workers to compete)
    bool submitTask(void (*execute)(void*), void* userData)
    {
        if (!initialized.load(std::memory_order_acquire) || execute == nullptr)
            return false;

        if (taskQueue.tryPush(execute, userData))
        {
            // wakeAllWorkers();
            wakeFirstWorker();
            return true;
        }
        return false;
    }

    // Execute a dependency graph - blocks until complete
    void executeDependencyGraph(DependencyTaskGraph* graph)
    {
        if (!initialized.load(std::memory_order_acquire))
            return;

        if (!graph || graph->empty())
            return;

        graph->setWakeCallback(
            []()
            {
                if (instance)
                    instance->wakeAllWorkers();
            }
        );

        graph->prepare();
        currentGraph.store(graph, std::memory_order_release);

        // // Wake first worker (cascades to wake others)
        // wakeFirstWorker();
        wakeAllWorkers();

        graph->waitUntilDone();

        currentGraph.store(nullptr, std::memory_order_release);
        graph->setWakeCallback(nullptr);
    }

    bool isCalledFromWorkerThread() const
    {
        auto currentId = std::this_thread::get_id();
        for (const auto& w : workers)
            if (w && w->getThreadId() == currentId)
                return true;
        return false;
    }

    // Wake all workers to check for work
    void wakeAllWorkers()
    {
        for (const auto& worker : workers)
            if (worker)
                worker->signal();
    }

    // Wake first worker (cascades to others via wakeNextWorker)
    void wakeFirstWorker()
    {
        if (!workers.empty() && workers[0])
            workers[0]->signal();
    }

private:
    class Worker
    {
    public:
        explicit Worker(RealtimeThreadPool& p, int workerIdx, int coreId = -1)
            : pool(p)
            , workerIndex(workerIdx)
            , thread(&Worker::run, this)
        {
            trySetRealtimePriority(thread);
            if (coreId >= 0)
                tryPinThreadToCore(thread, coreId);
        }

        ~Worker()
        {
            shouldExit.store(true, std::memory_order_release);
            wakeFlag.store(true, std::memory_order_release);
            spinAtomicNotifyOne(wakeFlag);
            if (thread.joinable())
                thread.join();
        }

        void waitUntilStarted()
        {
            while (!started.load(std::memory_order_acquire))
                std::this_thread::yield();
        }

        std::thread::id getThreadId() const
        {
            return thread.get_id();
        }

        void signal()
        {
            wakeFlag.store(true, std::memory_order_release);
            spinAtomicNotifyOne(wakeFlag);
        }

        void wakeNextWorker()
        {
            // Don't wake next worker if pool is shutting down
            if (!pool.initialized.load(std::memory_order_acquire))
                return;

            const int total = static_cast<int>(pool.workers.size());
            if (total <= 1)
                return;

            int nextIndex = (workerIndex + 1) % total;
            if (nextIndex == workerIndex)
                return;

            auto* nextWorker = pool.workers[nextIndex].get();
            if (nextWorker)
                nextWorker->signal();
        }

    private:
        void run()
        {
            started.store(true, std::memory_order_release);

            while (!shouldExit.load(std::memory_order_acquire))
            {
                spinAtomicWait(wakeFlag, false);
                wakeFlag.store(false, std::memory_order_relaxed);

                // Process tasks as long as there's work available
                bool didWork;
                do
                {
                    didWork = false;

                    // Check for dependency graph work
                    if (auto* graph = pool.currentGraph.load(std::memory_order_acquire))
                    {
                        wakeNextWorker();
                        if (graph->tryExecuteOneTask())
                        {
                            didWork = true;
                            continue;
                        }
                    }

                    // Check for fire-and-forget tasks
                    RealtimeTaskQueue::Task task;
                    if (pool.taskQueue.tryPop(task))
                    {
                        wakeNextWorker();
                        if (task.execute)
                            task.execute(task.userData);
                        didWork = true;
                    }
                } while (didWork);
            }
        }

        RealtimeThreadPool& pool;
        int workerIndex;
        std::atomic<bool> wakeFlag{false};
        std::atomic<bool> shouldExit{false};
        std::atomic<bool> started{false};
        std::thread thread;
    };

    RealtimeThreadPool() = default;

    inline static RealtimeThreadPool* instance = nullptr;

    std::vector<std::unique_ptr<Worker>> workers;
    RealtimeTaskQueue taskQueue;
    std::atomic<DependencyTaskGraph*> currentGraph{nullptr};
    std::atomic<bool> initialized{false};

    RealtimeThreadPool(const RealtimeThreadPool&) = delete;
    RealtimeThreadPool& operator=(const RealtimeThreadPool&) = delete;
};

} // namespace atk
