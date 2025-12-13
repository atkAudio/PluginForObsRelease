// Copyright (c) 2025 atkAudio
// Spin wait with exponential backoff (8→8192) then atomic::wait fallback

#pragma once

#include <atomic>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <emmintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
#include <intrin.h>
#endif

namespace atk
{

inline void cpuPause()
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

template <typename T>
void spinAtomicWait(std::atomic<T>& atomic, T oldValue, std::memory_order order = std::memory_order_acquire)
{
    for (int i = 0; i < 10; ++i)
    {
        if (atomic.load(order) != oldValue)
            return;

        for (int p = 0; p < (8 << i); ++p)
            cpuPause();
    }

    while (atomic.load(order) == oldValue)
        atomic.wait(oldValue, order);
}

template <typename T>
void spinAtomicNotifyOne(std::atomic<T>& atomic)
{
    atomic.notify_one();
}

template <typename T>
void spinAtomicNotifyAll(std::atomic<T>& atomic)
{
    atomic.notify_all();
}

} // namespace atk
