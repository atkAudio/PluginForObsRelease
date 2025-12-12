// Copyright (c) 2024 atkAudio

#pragma once

#include <thread>
#include <iostream>
#include <algorithm>

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
#include <mach/thread_policy.h>
#endif
#endif

namespace atk
{

// Try to pin a thread to a specific CPU core.
// Returns true if successful, false otherwise.
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
    // macOS: Use thread_policy_set with THREAD_AFFINITY_POLICY
    // Note: macOS affinity is more of a hint, not a hard binding
    thread_affinity_policy_data_t policy = {coreId};
    ok = thread_policy_set(
             pthread_mach_thread_np(t.native_handle()),
             THREAD_AFFINITY_POLICY,
             (thread_policy_t)&policy,
             THREAD_AFFINITY_POLICY_COUNT
         )
      == KERN_SUCCESS;

#else
    (void)t;
    (void)coreId;
#endif

    if (!ok)
        std::cerr << "[atk::pin] Failed to pin thread to core " << coreId << std::endl;
    return ok;
}

// Try to set realtime/high priority on a thread.
// Returns true if successful, false otherwise.
// On failure, the thread continues with normal priority.
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
    // macOS: Set highest SCHED_OTHER priority
    // True realtime on macOS requires Audio Unit workgroups
    sched_param param{};
    param.sched_priority = sched_get_priority_max(SCHED_OTHER);
    return pthread_setschedparam(t.native_handle(), SCHED_OTHER, &param) == 0;

#else
    (void)t;
    return false;
#endif
}

} // namespace atk
