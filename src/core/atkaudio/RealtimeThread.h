// Copyright (c) 2024 atkAudio

#pragma once

#include <thread>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#include <sched.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#endif
#ifdef __linux__
#include <cstring>
#endif
#endif

namespace atk
{

// Try to pin a thread to a specific CPU core.
// Returns true if successful, false otherwise.
// macOS does not support CPU pinning; affinity tags are not CPU IDs.
// coreId should be in range [0, hardware_concurrency-1]
inline bool tryPinThreadToCore(std::thread& t, int coreId) noexcept
{
    // Clamp coreId into a valid logical core range; wrap if out-of-range
    const auto logical = (std::max)(1u, std::thread::hardware_concurrency());
    if (coreId < 0)
        coreId = 0;
    if (coreId >= static_cast<int>(logical))
        coreId = coreId % static_cast<int>(logical);

    bool ok = false;
#ifdef _WIN32
    // Windows: Set thread affinity to a single core
    auto handle = t.native_handle();
    if (coreId < static_cast<int>(sizeof(DWORD_PTR) * 8))
    {
        DWORD_PTR mask = static_cast<DWORD_PTR>(1) << coreId;
        ok = SetThreadAffinityMask(handle, mask) != 0;
    }

#elif defined(__linux__)
    // Linux: Use pthread_setaffinity_np
    if (coreId < CPU_SETSIZE)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(coreId, &cpuset);
        ok = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset) == 0;
    }

#elif defined(__APPLE__)
    (void)t;
    std::cerr << "[atk::pin] CPU pinning is unsupported on macOS; use worker QoS instead" << std::endl;
    return false;

#else
    (void)t;
    (void)coreId;
#endif

    if (!ok)
        std::cerr << "[atk::pin] Failed to pin thread to core " << coreId << std::endl;
    return ok;
}

#ifdef __APPLE__
inline bool trySetCurrentThreadAudioQos() noexcept
{
    const int error = pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0);
    if (error != 0)
        std::cerr << "[atk::priority] Failed to set macOS default QoS (error " << error << ")" << std::endl;
    return error == 0;
}

inline bool trySetAudioDeadlinePriority(std::thread& thread, double sampleRate, int blockSize) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || blockSize <= 0)
    {
        std::cerr << "[atk::priority] Invalid audio deadline timing" << std::endl;
        return false;
    }

    mach_timebase_info_data_t timebase{};
    const auto timebaseResult = mach_timebase_info(&timebase);
    if (timebaseResult != KERN_SUCCESS || timebase.numer == 0 || timebase.denom == 0)
    {
        std::cerr << "[atk::priority] Unable to query Mach timebase (error " << timebaseResult << ")" << std::endl;
        return false;
    }

    const double durationSeconds = static_cast<double>(blockSize) / sampleRate;
    const auto ticks = static_cast<uint32_t>(std::clamp(
        durationSeconds * 1.0e9 * timebase.denom / timebase.numer,
        1.0,
        static_cast<double>((std::numeric_limits<uint32_t>::max)())
    ));

    thread_time_constraint_policy_data_t policy{};
    // Match JUCE RealtimeOptions::withApproximateAudioProcessingTime: no fixed period.
    policy.period = 0;
    policy.computation = ticks;
    policy.constraint = ticks;
    policy.preemptible = true;
    const auto result = thread_policy_set(
        pthread_mach_thread_np(thread.native_handle()),
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT
    );
    if (result != KERN_SUCCESS)
        std::cerr << "[atk::priority] Failed to set Mach audio deadline (error " << result << ")" << std::endl;
    return result == KERN_SUCCESS;
}

// QoS must be set by the worker itself, before it publishes its ready state.
inline bool trySetCurrentThreadRealtimePriority() noexcept
{
    const int error = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (error != 0)
        std::cerr << "[atk::priority] Failed to set macOS worker QoS (error " << error << ")" << std::endl;
    return error == 0;
}
#endif

#ifdef __linux__
inline int getLinuxRealtimeWorkerPriority() noexcept
{
    const int minimum = (std::max)(0, sched_get_priority_min(SCHED_RR));
    const int maximum = (std::max)(1, sched_get_priority_max(SCHED_RR));
    return minimum + (maximum - minimum) * 5 / 10;
}

inline bool trySetCurrentLinuxRealtimePriority() noexcept
{
    sched_param param{};
    param.sched_priority = getLinuxRealtimeWorkerPriority();
    const int error = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    if (error != 0)
        std::cerr
            << "[atk::priority] SCHED_RR priority "
            << param.sched_priority
            << " failed: "
            << std::strerror(error)
            << std::endl;
    return error == 0;
}
#endif

#ifdef _WIN32
inline bool trySetCurrentWindowsRealtimePriority() noexcept
{
    // Deliberately leave the hosting OBS process priority class unchanged.
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
        return true;
    std::cerr << "[atk::priority] TIME_CRITICAL worker priority failed (Win32 " << GetLastError() << ")" << std::endl;
    return false;
}
#endif

// Try to set realtime/high priority on a thread.
// Returns true if successful, false otherwise.
// On failure, the thread continues with normal priority.
// On macOS this must be called from the target thread and requests high QoS,
// not deadline-based realtime scheduling.
inline bool trySetRealtimePriority(std::thread& t) noexcept
{
#ifdef _WIN32
    // Windows: Try TIME_CRITICAL first, fall back to HIGHEST
    auto handle = t.native_handle();
    if (SetThreadPriority(handle, THREAD_PRIORITY_TIME_CRITICAL))
        return true;
    return SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST) != 0;

#elif defined(__linux__)
    // Linux: Try SCHED_RR (realtime), requires CAP_SYS_NICE or root
    sched_param param{};
    param.sched_priority = sched_get_priority_max(SCHED_RR);
    if (param.sched_priority > 0)
    {
        if (pthread_setschedparam(t.native_handle(), SCHED_RR, &param) == 0)
            return true;
    }
    // Fallback: try lower realtime priority
    param.sched_priority = sched_get_priority_min(SCHED_RR);
    return pthread_setschedparam(t.native_handle(), SCHED_RR, &param) == 0;

#elif defined(__APPLE__)
    if (!pthread_equal(t.native_handle(), pthread_self()))
    {
        std::cerr << "[atk::priority] macOS worker QoS must be set from the target thread" << std::endl;
        return false;
    }
    return trySetCurrentThreadRealtimePriority();

#else
    (void)t;
    return false;
#endif
}

} // namespace atk
