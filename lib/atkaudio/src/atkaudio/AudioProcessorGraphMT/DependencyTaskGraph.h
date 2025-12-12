// Copyright (c) 2025 atkAudio
// Dependency-based task graph for realtime processing

#pragma once

#include "../RealtimeThread.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#ifndef CPU_PAUSE
#if JUCE_INTEL || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#define CPU_PAUSE() _mm_pause()
#elif defined(__arm__) || defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
#include <intrin.h>
#define CPU_PAUSE() __yield()
#elif defined(__ARM_ACLE)
#include <arm_acle.h>
#define CPU_PAUSE() __yield()
#else
#define CPU_PAUSE() __asm__ __volatile__("yield")
#endif
#else
#define CPU_PAUSE() std::atomic_signal_fence(std::memory_order_seq_cst)
#endif
#endif

namespace atk
{

//==============================================================================
// Lock-free MPMC ready queue for task indices
template <typename T, size_t Capacity = 1024>
class LockFreeReadyQueue
{
public:
    LockFreeReadyQueue()
        : head(0)
        , tail(0)
    {
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
        for (size_t i = 0; i < Capacity; ++i)
            slots[i].sequence.store(i, std::memory_order_relaxed);
    }

    void reset()
    {
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < Capacity; ++i)
            slots[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool tryPush(T value)
    {
        size_t pos = head.load(std::memory_order_relaxed);
        for (;;)
        {
            Slot& slot = slots[pos & (Capacity - 1)];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0)
            {
                if (head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    slot.data = value;
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

    bool tryPop(T& value)
    {
        size_t pos = tail.load(std::memory_order_relaxed);
        for (;;)
        {
            Slot& slot = slots[pos & (Capacity - 1)];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0)
            {
                if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    value = slot.data;
                    slot.sequence.store(pos + Capacity, std::memory_order_release);
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
    struct Slot
    {
        std::atomic<size_t> sequence;
        T data;
    };

    alignas(64) std::atomic<size_t> head;
    alignas(64) std::atomic<size_t> tail;
    alignas(64) Slot slots[Capacity];
};

//==============================================================================
// Task node with atomic dependency counter
struct TaskNode
{
    void* userData = nullptr;
    void (*execute)(void* userData) = nullptr;
    std::atomic<int> pendingDependencies{0};
    int initialDependencyCount = 0;
    std::vector<size_t> dependentIndices;
    size_t taskIndex = 0;
    std::atomic<bool> completed{false};
    int preferredWorker = 0;

    explicit TaskNode(size_t index = 0)
        : taskIndex(index)
    {
    }

    TaskNode(const TaskNode&) = delete;
    TaskNode& operator=(const TaskNode&) = delete;
    TaskNode(TaskNode&&) = delete;
    TaskNode& operator=(TaskNode&&) = delete;

    void reset()
    {
        pendingDependencies.store(initialDependencyCount, std::memory_order_relaxed);
        completed.store(false, std::memory_order_relaxed);
    }

    bool isReady() const
    {
        return pendingDependencies.load(std::memory_order_acquire) == 0 && !completed.load(std::memory_order_acquire);
    }
};

//==============================================================================
// Dependency-based task graph for parallel audio processing
// Uses address-based hashing for task affinity:
// - Same userData pointer → same preferred worker → better cache locality
// - Work stealing ensures load balancing when workers finish early
class DependencyTaskGraph
{
public:
    static constexpr int kMaxWorkers = 32;

    DependencyTaskGraph() = default;

    void reserve(size_t maxTasks)
    {
        tasks.reserve(maxTasks);
    }

    void clear()
    {
        tasks.clear();
        for (int i = 0; i < kMaxWorkers; ++i)
            workerQueues[i].reset();
        completedCount.store(0, std::memory_order_relaxed);
        totalTasks = 0;
    }

    bool empty() const
    {
        return tasks.empty();
    }

    void setNumWorkers(int n)
    {
        numWorkers_ = (n > 0 && n <= kMaxWorkers) ? n : 1;
    }

    int getNumWorkers() const
    {
        return numWorkers_;
    }

    size_t addTask(void* userData, void (*execute)(void*), int dependencyCount = 0)
    {
        size_t index = tasks.size();
        auto task = std::make_unique<TaskNode>(index);
        task->userData = userData;
        task->execute = execute;
        task->initialDependencyCount = dependencyCount;
        task->pendingDependencies.store(dependencyCount, std::memory_order_relaxed);
        task->completed.store(false, std::memory_order_relaxed);
        tasks.push_back(std::move(task));
        return index;
    }

    void addDependency(size_t taskIndex, size_t dependsOnIndex)
    {
        if (dependsOnIndex < tasks.size() && taskIndex < tasks.size())
        {
            tasks[dependsOnIndex]->dependentIndices.push_back(taskIndex);
            tasks[taskIndex]->initialDependencyCount++;
            tasks[taskIndex]->pendingDependencies.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void prepare()
    {
        while (activeWorkers.load(std::memory_order_acquire) > 0)
            CPU_PAUSE();

        for (int i = 0; i < kMaxWorkers; ++i)
            workerQueues[i].reset();
        completedCount.store(0, std::memory_order_relaxed);
        totalTasks = tasks.size();

        const int nw = numWorkers_;
        for (size_t i = 0; i < tasks.size(); ++i)
        {
            tasks[i]->preferredWorker = computePreferredWorker(tasks[i]->userData, nw);
            tasks[i]->reset();
            if (tasks[i]->initialDependencyCount == 0)
                enqueueTask(i);
        }
    }

    void executeUntilDone()
    {
        executeUntilDoneForWorker(-1);

        // Wait for workers to exit before returning
        int spinCount = 8;
        while (activeWorkers.load(std::memory_order_acquire) > 0)
        {
            for (int i = 0; i < spinCount; ++i)
                CPU_PAUSE();
            if (spinCount < 8192)
                spinCount *= 2;
            else
                std::this_thread::yield();
        }
    }

    void executeUntilDoneForWorker(int workerId)
    {
        activeWorkers.fetch_add(1, std::memory_order_acq_rel);

        const int nw = numWorkers_;
        int spinCount = 8;
        while (completedCount.load(std::memory_order_acquire) < totalTasks)
        {
            size_t taskIndex;
            if (tryPopTask(taskIndex, workerId, nw))
            {
                executeTask(taskIndex);
                spinCount = 8;
            }
            else
            {
                for (int i = 0; i < spinCount; ++i)
                {
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                    CPU_PAUSE();
                    if (hasAnyWork(nw))
                        break;
                }

                if (completedCount.load(std::memory_order_acquire) >= totalTasks)
                    break;

                if (spinCount < 8192)
                    spinCount *= 2;
                else
                    std::this_thread::yield();
            }
        }

        activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
    }

    bool tryExecuteOne()
    {
        return tryExecuteOneForWorker(-1);
    }

    bool tryExecuteOneForWorker(int workerId)
    {
        size_t taskIndex;
        if (tryPopTask(taskIndex, workerId, numWorkers_))
        {
            executeTask(taskIndex);
            return true;
        }
        return false;
    }

    bool isComplete() const
    {
        return completedCount.load(std::memory_order_acquire) >= totalTasks;
    }

    size_t getTaskCount() const
    {
        return tasks.size();
    }

    size_t getCompletedCount() const
    {
        return completedCount.load(std::memory_order_acquire);
    }

    TaskNode& getTask(size_t index)
    {
        return *tasks[index];
    }

    const TaskNode& getTask(size_t index) const
    {
        return *tasks[index];
    }

private:
    int computePreferredWorker(void* ptr, int nw) const
    {
        if (nw <= 1 || ptr == nullptr)
            return 0;
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return static_cast<int>((addr >> 6) % static_cast<uintptr_t>(nw));
    }

    void enqueueTask(size_t taskIndex)
    {
        int worker = tasks[taskIndex]->preferredWorker;
        int spinCount = 8;
        while (!workerQueues[worker].tryPush(taskIndex))
        {
            for (int i = 0; i < spinCount; ++i)
                CPU_PAUSE();
            if (spinCount < 8192)
                spinCount *= 2;
            else
                std::this_thread::yield();
        }
    }

    bool tryPopTask(size_t& taskIndex, int workerId, int nw)
    {
        if (workerId >= 0 && workerId < nw)
            if (workerQueues[workerId].tryPop(taskIndex))
                return true;

        int start = (workerId >= 0) ? (workerId + 1) % nw : 0;
        for (int i = 0; i < nw; ++i)
        {
            int q = (start + i) % nw;
            if (workerQueues[q].tryPop(taskIndex))
                return true;
        }
        return false;
    }

    bool hasAnyWork(int nw) const
    {
        for (int i = 0; i < nw; ++i)
            if (!workerQueues[i].isEmpty())
                return true;
        return false;
    }

    void executeTask(size_t taskIndex)
    {
        TaskNode& task = *tasks[taskIndex];

        if (task.execute && task.userData)
            task.execute(task.userData);

        task.completed.store(true, std::memory_order_release);

        for (size_t depIndex : task.dependentIndices)
        {
            TaskNode& dependent = *tasks[depIndex];
            if (dependent.pendingDependencies.fetch_sub(1, std::memory_order_acq_rel) == 1)
                enqueueTask(depIndex);
        }

        completedCount.fetch_add(1, std::memory_order_release);
    }

    std::vector<std::unique_ptr<TaskNode>> tasks;
    LockFreeReadyQueue<size_t, 1024> workerQueues[kMaxWorkers];
    int numWorkers_ = 1;
    std::atomic<size_t> completedCount{0};
    std::atomic<int> activeWorkers{0};
    size_t totalTasks = 0;
};

} // namespace atk
