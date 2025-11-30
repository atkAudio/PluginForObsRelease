// Copyright (c) 2024 atkAudio

#pragma once

#include "../CpuInfo.h"
#include "../RealtimeThread.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

namespace atk
{

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

        void run()
        {
            if (execute && userData)
                execute(userData);
        }
    };

    void initialize(int numThreads = 0, int maxJobs = 1024)
    {
        if (initialized.load(std::memory_order_acquire))
            return;

        std::lock_guard<std::mutex> lock(initMutex);
        if (initialized.load(std::memory_order_acquire))
            return;

        if (numThreads <= 0)
        {
            // Reserve 2 cores: 1 for main audio thread, 1 for system/GUI
            numThreads = (std::max)(1, getNumPhysicalCpus() - 2);
        }

        jobs.resize(maxJobs);
        head.store(0);
        tail.store(0);

        for (int i = 0; i < numThreads; ++i)
            workers.push_back(std::make_unique<Worker>(*this));

        initialized.store(true, std::memory_order_release);
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(initMutex);
        if (!initialized.load(std::memory_order_acquire))
            return;

        workers.clear();
        initialized.store(false, std::memory_order_release);
    }

    bool isReady() const
    {
        return initialized.load(std::memory_order_acquire);
    }

    void addJob(void (*exec)(void*), void* data)
    {
        size_t idx = head.fetch_add(1, std::memory_order_acq_rel) % jobs.size();
        jobs[idx] = {exec, data};
    }

    void kickWorkers()
    {
        if (!workers.empty())
            workers[0]->wakeUp();
    }

private:
    class Worker
    {
    public:
        explicit Worker(SecondaryThreadPool& p)
            : pool(p)
            , thread(&Worker::run, this)
        {
            // Try to set realtime priority, ignore failure (falls back to normal)
            trySetRealtimePriority(thread);
        }

        ~Worker()
        {
            shouldExit.store(true, std::memory_order_release);
            wakeUp();
            if (thread.joinable())
                thread.join();
        }

        void wakeUp()
        {
            pending.store(true, std::memory_order_release);
            cv.notify_one();
        }

    private:
        void run()
        {
            while (!shouldExit.load(std::memory_order_acquire))
            {
                // Process all available jobs
                for (;;)
                {
                    int t = pool.tail.load(std::memory_order_acquire);
                    int h = pool.head.load(std::memory_order_acquire);
                    if (t >= h)
                        break;
                    if (pool.tail.compare_exchange_weak(t, t + 1, std::memory_order_acq_rel))
                        pool.jobs[t % pool.jobs.size()].run();
                }

                // Wait for work
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(
                    lock,
                    [this]
                    { return pending.load(std::memory_order_acquire) || shouldExit.load(std::memory_order_acquire); }
                );
                pending.store(false, std::memory_order_release);
            }
        }

        SecondaryThreadPool& pool;
        std::thread thread;
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> pending{false};
        std::atomic<bool> shouldExit{false};
    };

    SecondaryThreadPool() = default;

public:
    ~SecondaryThreadPool()
    {
        shutdown();
    }

private:
    inline static SecondaryThreadPool* instance = nullptr;

    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<Job> jobs;
    std::atomic<int> head{0};
    std::atomic<int> tail{0};
    std::mutex initMutex;
    std::atomic<bool> initialized{false};

    SecondaryThreadPool(const SecondaryThreadPool&) = delete;
    SecondaryThreadPool& operator=(const SecondaryThreadPool&) = delete;
};

} // namespace atk
