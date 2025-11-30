// Copyright (c) 2024 atkAudio

#pragma once

#include <atomic>
#include <chrono>

namespace atk
{

/**
    Measures CPU usage for variable-buffer-size audio processing.

    Unlike JUCE's AudioDeviceManager::getCpuUsage() which assumes fixed buffer sizes,
    this meter calculates the ratio of actual processing time to available time
    based on each callback's actual buffer size, making it accurate for variable
    buffer sizes.

    Shows instantaneous load with peak hold (3 seconds).

    Usage:
        - Call start() at the beginning of audio callback
        - Call stop(numSamples, sampleRate) at the end
        - Call getLoad() from UI thread to display (returns peak-held value)

    Thread safety:
        - start()/stop(): Called from audio thread only
        - getLoad(): Safe to call from any thread (returns atomic value)
*/
class CpuMeter
{
public:
    CpuMeter() = default;

    /** Call at the start of audio processing */
    void start()
    {
        startTime = std::chrono::high_resolution_clock::now();
    }

    /** Call at the end of audio processing with actual buffer size */
    void stop(int numSamples, double sampleRate)
    {
        if (sampleRate <= 0.0 || numSamples <= 0)
            return;

        auto endTime = std::chrono::high_resolution_clock::now();
        auto processingTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count();

        // Calculate available time for this buffer in nanoseconds
        double availableTimeNs = (numSamples / sampleRate) * 1e9;

        // Calculate load for this callback
        float thisLoad = static_cast<float>(processingTimeNs / availableTimeNs);

        // Store instantaneous load
        currentLoad.store(thisLoad, std::memory_order_relaxed);

        // Update peak with 3-second hold
        auto now = std::chrono::steady_clock::now();
        float peak = peakLoad.load(std::memory_order_relaxed);

        if (thisLoad >= peak)
        {
            peakLoad.store(thisLoad, std::memory_order_relaxed);
            peakTime = now;
        }
        else
        {
            // Check if 3 seconds have passed since peak
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - peakTime).count();
            if (elapsed > 3000)
            {
                peakLoad.store(thisLoad, std::memory_order_relaxed);
                peakTime = now;
            }
        }
    }

    /** Get current CPU load with peak hold (0.0 to 1.0+) - thread safe */
    float getLoad() const
    {
        return peakLoad.load(std::memory_order_relaxed);
    }

    /** Get instantaneous load without peak hold */
    float getInstantLoad() const
    {
        return currentLoad.load(std::memory_order_relaxed);
    }

    /** Reset the meter */
    void reset()
    {
        currentLoad.store(0.0f, std::memory_order_relaxed);
        peakLoad.store(0.0f, std::memory_order_relaxed);
    }

private:
    std::chrono::high_resolution_clock::time_point startTime;
    std::chrono::steady_clock::time_point peakTime;
    std::atomic<float> currentLoad{0.0f};
    std::atomic<float> peakLoad{0.0f};
};

} // namespace atk
