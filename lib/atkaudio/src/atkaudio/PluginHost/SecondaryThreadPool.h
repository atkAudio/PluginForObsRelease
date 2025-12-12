// Copyright (c) 2024 atkAudio

#pragma once

#include "../CpuInfo.h"
#include "../RealtimeThread.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <juce_core/juce_core.h>

#if defined(_MSC_VER)
#if defined(_M_X64)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif
#endif

namespace atk
{

namespace detail
{
inline void cpuPause()
{
#if defined(_MSC_VER)
#if defined(_M_X64)
    _mm_pause();
#elif defined(_M_ARM64)
    __yield();
#else
    std::this_thread::yield();
#endif
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
#else
    std::this_thread::yield();
#endif
}

inline void spinWait(int count)
{
    for (int i = 0; i < count; ++i)
    {
        cpuPause();
        std::atomic_signal_fence(std::memory_order_acq_rel);
    }
}
} // namespace detail

class SpinLock
{
public:
    void lock()
    {
        if (flag.test_and_set(std::memory_order_acquire))
        {
            int spinCount = 8;
            constexpr int maxSpinCount = 8192;

            while (flag.test_and_set(std::memory_order_acquire))
            {
                if (spinCount < maxSpinCount)
                {
                    detail::spinWait(spinCount);
                    spinCount *= 2;
                }
                else
                {
                    DBG("SpinLock: max spin count reached, yielding");
                    std::this_thread::yield();
                }
            }
        }
    }

    void unlock()
    {
        flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
};

class SecondaryThreadPool
{
public:
    static SecondaryThreadPool* getInstance()
    {
        if (!instance)
            instance = new SecondaryThreadPool();
        return instance;
    }

    static void deleteInstance()
    {
        delete instance;
        instance = nullptr;
    }

    struct Job
    {
        void (*execute)(void*) = nullptr;
        void* userData = nullptr;
    };

    void initialize(int numThreads = 0, int maxJobs = 1024)
    {
        if (initialized.load(std::memory_order_acquire))
            return;

        std::lock_guard<std::mutex> initLock(initMutex);
        if (initialized.load(std::memory_order_acquire))
            return;

        if (numThreads <= 0)
            numThreads = (std::max)(1, getNumPhysicalCpus() - 2);

        jobs.resize(maxJobs);
        head = 0;
        tail = 0;
        capacity = maxJobs;

        shouldExit.store(false, std::memory_order_relaxed);

        for (int i = 0; i < numThreads; ++i)
            workers.emplace_back(&SecondaryThreadPool::workerRun, this);

        for (auto& w : workers)
            trySetRealtimePriority(w);

        initialized.store(true, std::memory_order_release);
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> initLock(initMutex);
        if (!initialized.load(std::memory_order_acquire))
            return;

        {
            std::lock_guard<std::mutex> lock(wakeMutex);
            shouldExit.store(true, std::memory_order_release);
        }
        wakeCV.notify_all();

        for (auto& w : workers)
            if (w.joinable())
                w.join();
        workers.clear();

        initialized.store(false, std::memory_order_release);
    }

    bool isReady() const
    {
        return initialized.load(std::memory_order_acquire);
    }

    bool addJob(void (*exec)(void*), void* data)
    {
        queueLock.lock();

        unsigned int nextHead = (head + 1) % capacity;
        if (nextHead == tail)
        {
            queueLock.unlock();
            return false; // Queue full
        }

        jobs[head] = {exec, data};
        head = nextHead;

        queueLock.unlock();
        return true;
    }

    void kickWorkers()
    {
        wakeCV.notify_one();
    }

private:
    bool tryDequeue(Job& outJob)
    {
        queueLock.lock();

        if (tail == head)
        {
            queueLock.unlock();
            return false; // Queue empty
        }

        outJob = jobs[tail];
        tail = (tail + 1) % capacity;

        queueLock.unlock();
        return true;
    }

    void workerRun()
    {
        while (!shouldExit.load(std::memory_order_acquire))
        {
            Job job;
            while (tryDequeue(job))
                if (job.execute && job.userData)
                    job.execute(job.userData);

            {
                std::unique_lock<std::mutex> lock(wakeMutex);
                wakeCV.wait(lock, [this] { return shouldExit.load(std::memory_order_acquire) || tail != head; });
            }
        }
    }

    SecondaryThreadPool() = default;

public:
    ~SecondaryThreadPool()
    {
        shutdown();
    }

private:
    inline static SecondaryThreadPool* instance = nullptr;

    std::vector<std::thread> workers;
    std::vector<Job> jobs;
    unsigned int head = 0;
    unsigned int tail = 0;
    unsigned int capacity = 0;
    SpinLock queueLock;
    std::mutex initMutex;
    std::mutex wakeMutex;
    std::condition_variable wakeCV;
    std::atomic<bool> shouldExit{false};
    std::atomic<bool> initialized{false};

    SecondaryThreadPool(const SecondaryThreadPool&) = delete;
    SecondaryThreadPool& operator=(const SecondaryThreadPool&) = delete;
};

} // namespace atk
