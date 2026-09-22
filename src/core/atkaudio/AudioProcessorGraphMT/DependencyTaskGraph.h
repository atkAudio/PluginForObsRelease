// Copyright (c) 2025 atkAudio
// Dependency-based task graph for realtime parallel processing
//
// Features:
// - Lock-free MPMC queue for task scheduling
// - Dynamic preferred child: heaviest ready child executes on same thread
// - EMA-smoothed execution times for chain selection

#pragma once

#include "SpinWait.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace atk
{

//==============================================================================
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
struct TaskNode
{
    void* userData = nullptr;
    void (*execute)(void* userData) = nullptr;
    std::atomic<int> pendingDependencies{0};
    int initialDependencyCount = 0;
    std::vector<size_t> dependentIndices;
    size_t taskIndex = 0;
    std::chrono::nanoseconds executionTimeEma{0};

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
    }
};

//==============================================================================
class DependencyTaskGraph
{
public:
    using WakeCallback = void (*)();

    DependencyTaskGraph() = default;

    void reserve(size_t maxTasks)
    {
        tasks.reserve(maxTasks);
    }

    void clear()
    {
        tasks.clear();
        readyQueue.reset();
        completedCount.store(0, std::memory_order_relaxed);
        totalTasks = 0;
    }

    bool empty() const
    {
        return tasks.empty();
    }

    void setWakeCallback(WakeCallback callback)
    {
        wakeCallback = callback;
    }

    size_t addTask(void* userData, void (*execute)(void*), int dependencyCount = 0)
    {
        size_t index = tasks.size();
        auto task = std::make_unique<TaskNode>(index);
        task->userData = userData;
        task->execute = execute;
        task->initialDependencyCount = dependencyCount;
        task->pendingDependencies.store(dependencyCount, std::memory_order_relaxed);
        tasks.push_back(std::move(task));
        return index;
    }

    void addDependency(size_t taskIndex, size_t dependsOnIndex)
    {
        if (dependsOnIndex >= tasks.size() || taskIndex >= tasks.size())
            return;

        tasks[dependsOnIndex]->dependentIndices.push_back(taskIndex);
        tasks[taskIndex]->initialDependencyCount++;
        tasks[taskIndex]->pendingDependencies.fetch_add(1, std::memory_order_relaxed);
    }

    void prepare()
    {
        readyQueue.reset();
        completedCount.store(0, std::memory_order_relaxed);
        waitFlag.store(false, std::memory_order_relaxed);
        totalTasks = tasks.size();

        // Reset all tasks and push roots to ready queue
        for (size_t i = 0; i < tasks.size(); ++i)
        {
            tasks[i]->reset();
            if (tasks[i]->initialDependencyCount == 0)
                readyQueue.tryPush(i);
        }
    }

    void waitUntilDone()
    {
        while (!isComplete())
        {
            if (!tryExecuteOneTask())
            {
                size_t completed = completedCount.load(std::memory_order_acquire);
                if (completed < totalTasks)
                    spinAtomicWaitRealtime(completedCount, completed);
            }
        }
    }

    bool tryExecuteOneTask()
    {
        size_t taskIndex;
        if (readyQueue.tryPop(taskIndex))
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

    bool hasWork() const
    {
        return !readyQueue.isEmpty();
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
    void executeTask(size_t taskIndex)
    {
        TaskNode& task = *tasks[taskIndex];

        if (task.execute && task.userData)
            task.execute(task.userData);
        bool pushedToQueue = false;

        for (size_t depIndex : task.dependentIndices)
        {
            TaskNode& dependent = *tasks[depIndex];
            if (dependent.pendingDependencies.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                readyQueue.tryPush(depIndex);
                pushedToQueue = true;
            }
        }

        // Wake workers if we pushed any tasks to the queue
        if (pushedToQueue && wakeCallback)
            wakeCallback();

        // mark this task as completed
        if (completedCount.fetch_add(1, std::memory_order_acq_rel) + 1 >= totalTasks)
        {
            waitFlag.store(true, std::memory_order_release);
            spinAtomicNotifyOne(waitFlag);
        }
    }

    std::vector<std::unique_ptr<TaskNode>> tasks;
    LockFreeReadyQueue<size_t, 1024> readyQueue;
    std::atomic<size_t> completedCount{0};
    size_t totalTasks = 0;
    std::atomic<bool> waitFlag{false};
    WakeCallback wakeCallback = nullptr;
};

} // namespace atk
