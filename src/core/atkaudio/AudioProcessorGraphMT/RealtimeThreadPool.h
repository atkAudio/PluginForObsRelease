// Copyright (c) 2025 atkAudio

#pragma once

#include "../CpuInfo.h"
#include <atkaudio/Logging.h>
#include "../RealtimeThread.h"
#include "DependencyTaskGraph.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
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
private:
    class DispatchReaderGate
    {
    public:
        void open()
        {
            state.store(0, std::memory_order_release);
        }

        bool tryAcquire()
        {
            int current = state.load(std::memory_order_acquire);
            while (current >= 0)
                if (state.compare_exchange_weak(
                        current,
                        current + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire
                    ))
                    return true;

            return false;
        }

        void release()
        {
            int current = state.load(std::memory_order_acquire);
            for (;;)
            {
                int released = current >= 0 ? current - 1 : current + 1;
                if (state.compare_exchange_weak(current, released, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    if (released == closedAndDrained)
                        spinAtomicNotifyAll(state);
                    return;
                }
            }
        }

        void close()
        {
            int current = state.load(std::memory_order_acquire);
            while (current >= 0)
            {
                int closed = -current - 1;
                if (state.compare_exchange_weak(current, closed, std::memory_order_acq_rel, std::memory_order_acquire))
                    return;
            }
        }

        void waitUntilDrained()
        {
            int current = state.load(std::memory_order_acquire);
            while (current != closedAndDrained)
            {
                spinAtomicWaitRealtime(state, current);
                current = state.load(std::memory_order_acquire);
            }
        }

    private:
        // Non-negative values are open reader counts; -count-1 closes the gate while preserving active leases.
        static constexpr int closedAndDrained = -1;
        std::atomic<int> state{closedAndDrained};
    };

    struct DispatchSlot
    {
        std::atomic<bool> claimed{false};
        DispatchReaderGate readers;
        std::atomic<DependencyTaskGraph*> graph{nullptr};
    };

    class DispatchSlotLease
    {
    public:
        DispatchSlotLease() = default;

        DispatchSlotLease(DispatchReaderGate* readers_, DependencyTaskGraph* graph_)
            : readers(readers_)
            , graph(graph_)
        {
        }

        ~DispatchSlotLease()
        {
            if (readers != nullptr)
                readers->release();
        }

        DispatchSlotLease(const DispatchSlotLease&) = delete;
        DispatchSlotLease& operator=(const DispatchSlotLease&) = delete;

        DependencyTaskGraph* get() const
        {
            return graph;
        }

    private:
        DispatchReaderGate* readers = nullptr;
        DependencyTaskGraph* graph = nullptr;
    };

public:
    static constexpr int kMaxWorkers = 32;
    static constexpr size_t kMaxConcurrentDependencyGraphs = 64;

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

    ~RealtimeThreadPool()
    {
        shutdown();
    }

    void initialize(int numWorkers = 0)
    {
        if (initialized.load(std::memory_order_acquire))
            return;

        if (numWorkers <= 0)
            numWorkers = (std::max)(1, getNumPhysicalCpus() - 2);

        auto physicalCores = getPhysicalCoreMapping();
        const int numPhysical = static_cast<int>(physicalCores.size());

        atk::logging::info(
            "RealtimeThreadPool::initialize",
            juce::String::formatted(
                "initializing worker pool with %d workers (physical cores: %d)",
                numWorkers,
                numPhysical
            )
        );
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
    }

    bool isReady() const
    {
        return initialized.load(std::memory_order_acquire);
    }

    int getNumWorkers() const
    {
        return static_cast<int>(workers.size());
    }

    // Submit a fire-and-forget task and wake one worker to consume it.
    bool submitTask(void (*execute)(void*), void* userData)
    {
        if (!initialized.load(std::memory_order_acquire) || execute == nullptr)
            return false;

        if (taskQueue.tryPush(execute, userData))
        {
            wakeFirstWorker();
            return true;
        }
        return false;
    }

    // Executes a dependency graph to completion. Returns false when every fixed dispatch slot is busy.
    bool executeDependencyGraph(DependencyTaskGraph* graph)
    {
        if (!initialized.load(std::memory_order_acquire))
            return false;

        if (!graph || graph->empty())
            return true;

        auto* slot = claimDispatchSlot();
        if (slot == nullptr)
            return false;

        graph->setWakeCallback(
            []()
            {
                if (instance)
                    instance->wakeAllWorkers();
            }
        );

        graph->prepare();
        slot->graph.store(graph, std::memory_order_release);
        slot->readers.open();
        wakeAllWorkers();

        graph->waitUntilDone();

        slot->readers.close();
        slot->readers.waitUntilDrained();
        slot->graph.store(nullptr, std::memory_order_release);
        graph->setWakeCallback(nullptr);
        slot->claimed.store(false, std::memory_order_release);
        return true;
    }

    bool isCalledFromWorkerThread() const
    {
        auto currentId = std::this_thread::get_id();
        for (const auto& w : workers)
            if (w && w->getThreadId() == currentId)
                return true;
        return false;
    }

    void wakeAllWorkers()
    {
        for (const auto& worker : workers)
            if (worker)
                worker->signal();
    }

    void wakeFirstWorker()
    {
        if (!workers.empty() && workers[0])
            workers[0]->signal();
    }

private:
    DispatchSlot* claimDispatchSlot()
    {
        for (auto& slot : dispatchSlots)
        {
            bool expected = false;
            if (slot.claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return &slot;
        }

        return nullptr;
    }

    DispatchSlotLease acquireDispatchSlotGraph(DispatchSlot& slot)
    {
        if (!slot.readers.tryAcquire())
            return {};

        auto* graph = slot.graph.load(std::memory_order_acquire);
        if (graph != nullptr)
            return DispatchSlotLease(&slot.readers, graph);

        slot.readers.release();
        return {};
    }

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
            signal();
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
            [[maybe_unused]] ScopedRealtimeSanitizerDisabler disabler;
            spinAtomicNotifyOne(wakeFlag);
        }

        void wakeNextWorker()
        {
            if (!pool.initialized.load(std::memory_order_acquire))
                return;

            const int total = static_cast<int>(pool.workers.size());
            if (total <= 1)
                return;

            const int nextIndex = (workerIndex + 1) % total;
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

                    for (size_t offset = 0; offset < kMaxConcurrentDependencyGraphs; ++offset)
                    {
                        auto& slot = pool.dispatchSlots[(nextDispatchSlot + offset) % kMaxConcurrentDependencyGraphs];
                        auto lease = pool.acquireDispatchSlotGraph(slot);
                        if (auto* graph = lease.get())
                        {
                            if (graph->tryExecuteOneTask())
                            {
                                didWork = true;
                                wakeNextWorker();
                            }

                            if (didWork)
                            {
                                nextDispatchSlot = (nextDispatchSlot + offset + 1) % kMaxConcurrentDependencyGraphs;
                                break;
                            }
                        }
                    }

                    if (didWork)
                        continue;

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
        size_t nextDispatchSlot = 0;
        std::thread thread;
    };

    RealtimeThreadPool() = default;

    inline static RealtimeThreadPool* instance = nullptr;

    std::vector<std::unique_ptr<Worker>> workers;
    RealtimeTaskQueue taskQueue;
    std::array<DispatchSlot, kMaxConcurrentDependencyGraphs> dispatchSlots;
    std::atomic<bool> initialized{false};

    RealtimeThreadPool(const RealtimeThreadPool&) = delete;
    RealtimeThreadPool& operator=(const RealtimeThreadPool&) = delete;
};

} // namespace atk
