// Copyright (c) 2024 atkAudio

#pragma once

#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#include <sched.h>
#endif

namespace atk
{

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
