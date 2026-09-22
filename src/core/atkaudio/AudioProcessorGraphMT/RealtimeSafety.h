#pragma once

#include <thread>

#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#define ATK_RTSAN_ENABLED 1
#endif
#endif
#ifndef ATK_RTSAN_ENABLED
#define ATK_RTSAN_ENABLED 0
#endif

#if ATK_RTSAN_ENABLED && defined(__has_cpp_attribute) && __has_cpp_attribute(clang::nonblocking)
#define ATK_RTSAN_NONBLOCKING [[clang::nonblocking]]
#else
#define ATK_RTSAN_NONBLOCKING
#endif

#if ATK_RTSAN_ENABLED
#include <sanitizer/rtsan_interface.h>
#endif

namespace atk
{
class ScopedRealtimeSanitizerDisabler
{
public:
#if ATK_RTSAN_ENABLED
    __rtsan::ScopedDisabler disabler;
#endif
};

inline void rtsanDisabledYield()
{
    [[maybe_unused]] ScopedRealtimeSanitizerDisabler disabler;
    std::this_thread::yield();
}
} // namespace atk
