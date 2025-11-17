#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <juce_core/juce_core.h>

// CPU intrinsics for realtime-safe busy-waiting
#if JUCE_INTEL
#include <emmintrin.h> // _mm_pause()
#define CPU_PAUSE() _mm_pause()
#elif JUCE_ARM
#if JUCE_WINDOWS
// Windows ARM64 uses MSVC intrinsics
#include <intrin.h>
#define CPU_PAUSE() __yield()
#else
// Linux/macOS ARM uses GCC/Clang inline assembly or arm_acle.h
#if defined(__ARM_ACLE)
#include <arm_acle.h>
#define CPU_PAUSE() __yield()
#else
#define CPU_PAUSE() __asm__ __volatile__("yield")
#endif
#endif
#else
// Fallback: compiler barrier to prevent optimization
#define CPU_PAUSE() std::atomic_signal_fence(std::memory_order_seq_cst)
#endif

namespace atk
{

/**
 * Adaptive Exponential Backoff Spin Lock
 *
 * Three modes:
 * - Fixed1024: Legacy fixed 1024 pause cycles
 * - BenchmarkedAdaptive: Benchmarked exponential backoff based on buffer size/sample rate
 * - Fixed8192Backoff (default): Fixed exponential backoff 8→16384, then yield forever
 */
class AdaptiveSpinLock
{
public:
    enum class Mode
    {
        Fixed1024,           // Fixed 1024 pause cycles (legacy mode)
        BenchmarkedAdaptive, // Benchmarked exponential backoff based on buffer size
        Fixed8192Backoff     // Fixed exponential backoff up to 8192 (default, realtime-safe)
    };

    AdaptiveSpinLock(Mode mode = Mode::Fixed8192Backoff)
        : mode_(mode)
        , maxIterations_(11)
        , fixedSpinPauseCount_(1024)
    {
        if (mode_ == Mode::BenchmarkedAdaptive)
            benchmarkAndConfigureDefaults();
        else if (mode_ == Mode::Fixed1024)
            configureFixedSpinWait();
    }

    void configure(int samplesPerBlock, double sampleRate)
    {
        if (mode_ == Mode::Fixed1024)
        {
            configureFixedSpinWait();
            return;
        }
        else if (mode_ == Mode::Fixed8192Backoff)
        {
            return;
        }

        ensureBenchmarked();
        const double bufferTimeSeconds = samplesPerBlock / sampleRate;
        const double maxSpinTimeSeconds = bufferTimeSeconds / 2.0;
        const double maxSpinTimeNanoseconds = maxSpinTimeSeconds * 1e9;
        maxIterations_ = calculateMaxIterations(maxSpinTimeNanoseconds);
    }

    template <typename Predicate>
    void wait(Predicate predicate)
    {
        if (mode_ == Mode::Fixed1024)
        {
            while (!predicate())
                for (volatile int i = 0; i < fixedSpinPauseCount_; ++i)
                    CPU_PAUSE();
        }
        else if (mode_ == Mode::BenchmarkedAdaptive)
        {
            int iteration = 0;
            while (!predicate())
            {
                if (iteration >= maxIterations_)
                {
                    juce::Thread::yield();
                }
                else
                {
                    const int pauseCount = (8 << iteration);
                    for (volatile int i = 0; i < pauseCount;)
                    {
                        CPU_PAUSE();
                        i = i + 1;
                    }
                    ++iteration;
                }
            }
        }
        else // Mode::Fixed8192Backoff (default, now max 8192)
        {
            int iteration = 0;
            while (!predicate())
            {
                if (iteration > 10) // Max: 8192 pauses (~123μs @ 15ns/pause)
                {
                    juce::Thread::yield();
                }
                else
                {
                    const int pauseCount = (8 << iteration);
                    for (volatile int i = 0; i < pauseCount;)
                    {
                        CPU_PAUSE();
                        i = i + 1;
                    }
                    ++iteration;
                }
            }
        }
    }

    template <typename T>
    void
    waitFor(const std::atomic<T>& condition, T expectedValue, std::memory_order memoryOrder = std::memory_order_acquire)
    {
        wait([&condition, expectedValue, memoryOrder]() { return condition.load(memoryOrder) == expectedValue; });
    }

    template <typename T>
    void waitWhile(
        const std::atomic<T>& condition,
        T unwantedValue,
        std::memory_order memoryOrder = std::memory_order_acquire
    )
    {
        wait([&condition, unwantedValue, memoryOrder] { return condition.load(memoryOrder) != unwantedValue; });
    }

    static void spinWait(int pauseCount = 4)
    {
        for (volatile int i = 0; i < pauseCount;)
        {
            CPU_PAUSE();
            i = i + 1;
        }
    }

private:
    static void ensureBenchmarked()
    {
        bool expected = false;
        if (globalBenchmarked_.compare_exchange_strong(expected, true, std::memory_order_acquire))
        {
            const double result = benchmarkIterationLatencyOnRealtimeThread();
            globalAvgIterationNanoseconds_.store(result, std::memory_order_release);
        }
    }

    void benchmarkAndConfigureDefaults()
    {
        ensureBenchmarked();
        const double oneMsNanoseconds = 1000000.0;
        maxIterations_ = calculateMaxIterations(oneMsNanoseconds);
    }

    void configureFixedSpinWait()
    {
        ensureBenchmarked();
        constexpr double targetNanoseconds = 50000.0;
        const double avgPauseNs = getAvgPauseNanoseconds();
        const int calculatedPauseCount = static_cast<int>(targetNanoseconds / avgPauseNs);
        fixedSpinPauseCount_ = std::max(1024, calculatedPauseCount);
    }

    static double getAvgPauseNanoseconds()
    {
        ensureBenchmarked();
        constexpr int maxIterationToTest = 10;
        constexpr int totalPauses = 8184;
        const double avgIterationTime = globalAvgIterationNanoseconds_.load(std::memory_order_acquire);
        const double totalTimeForSequence = avgIterationTime * maxIterationToTest;
        return totalTimeForSequence / totalPauses;
    }

    int calculateMaxIterations(double timeBudgetNanoseconds) const
    {
        const double avgPauseNs = getAvgPauseNanoseconds();
        double cumulativeTime = 0.0;
        int iteration = 0;

        while (cumulativeTime < timeBudgetNanoseconds && iteration < 100)
        {
            const long long pauseCount = (8LL << iteration);
            const double iterationTime = pauseCount * avgPauseNs;
            if (cumulativeTime + iterationTime > timeBudgetNanoseconds)
                break;
            cumulativeTime += iterationTime;
            ++iteration;
        }
        return std::max(1, iteration);
    }

    static double benchmarkIterationLatencyOnRealtimeThread()
    {
        std::atomic<double> result{0.0};

        class BenchmarkThread : public juce::Thread
        {
        public:
            explicit BenchmarkThread(std::atomic<double>& result)
                : juce::Thread("AdaptiveSpinLock Benchmark")
                , result_(result)
            {
            }

            void run() override
            {
                result_.store(benchmarkIterationLatency(), std::memory_order_release);
            }

        private:
            std::atomic<double>& result_;
        };

        BenchmarkThread benchmarkThread(result);
        juce::Thread::RealtimeOptions options = juce::Thread::RealtimeOptions().withPriority(8);
        benchmarkThread.startRealtimeThread(options);
        benchmarkThread.stopThread(-1);
        return result.load(std::memory_order_acquire);
    }

    static double benchmarkIterationLatency()
    {
        constexpr int numSamples = 3;
        constexpr int maxIterationToTest = 10;

        auto start = std::chrono::high_resolution_clock::now();
        for (int sample = 0; sample < numSamples; ++sample)
        {
            for (int iteration = 0; iteration < maxIterationToTest; ++iteration)
            {
                const int pauseCount = (8 << iteration);
                for (volatile int i = 0; i < pauseCount;)
                {
                    CPU_PAUSE();
                    i = i + 1;
                }
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

        const double totalIterations = numSamples * maxIterationToTest;
        const double avgNanoseconds = duration.count() / totalIterations;
        return std::max(avgNanoseconds, 1.0);
    }

    alignas(8) inline static std::atomic<bool> globalBenchmarked_{false};
    alignas(8) inline static std::atomic<double> globalAvgIterationNanoseconds_{1000.0};

    Mode mode_;
    int maxIterations_;
    int fixedSpinPauseCount_;
};

} // namespace atk

#undef CPU_PAUSE
