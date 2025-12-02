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

    Shows load with 3-second peak hold.

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
        startTime = std::chrono::steady_clock::now();
    }

    /** Call at the end of audio processing with actual buffer size */
    void stop(int numSamples, double sampleRate)
    {
        if (sampleRate <= 0.0 || numSamples <= 0)
            return;

        auto endTime = std::chrono::steady_clock::now();
        auto processingNs = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count();
        double availableNs = (numSamples / sampleRate) * 1e9;
        float load = static_cast<float>(processingNs / availableNs);

        // Update peak with 3-second hold
        auto now = std::chrono::steady_clock::now();
        float peak = peakLoad.load(std::memory_order_relaxed);

        if (load >= peak)
        {
            peakLoad.store(load, std::memory_order_relaxed);
            peakTime = now;
        }
        else if (std::chrono::duration_cast<std::chrono::seconds>(now - peakTime).count() >= 3)
        {
            peakLoad.store(load, std::memory_order_relaxed);
            peakTime = now;
        }
    }

    /** Get current CPU load with peak hold (0.0 to 1.0+) - thread safe */
    float getLoad() const
    {
        return peakLoad.load(std::memory_order_relaxed);
    }

    /** Reset the meter */
    void reset()
    {
        peakLoad.store(0.0f, std::memory_order_relaxed);
    }

private:
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point peakTime;
    std::atomic<float> peakLoad{0.0f};
};

} // namespace atk