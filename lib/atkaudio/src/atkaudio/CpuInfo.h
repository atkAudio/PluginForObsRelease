// Copyright (c) 2024 atkAudio

#pragma once

#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <cstdio>
#include <cstring>
#include <cstdlib>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace atk
{

// Returns the number of physical CPU cores (not hyper-threaded logical cores)
inline int getNumPhysicalCpus() noexcept
{
#ifdef _WIN32
    DWORD bufferSize = 0;
    GetLogicalProcessorInformation(nullptr, &bufferSize);

    const auto numBuffers = bufferSize / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    if (numBuffers == 0)
        return static_cast<int>(std::thread::hardware_concurrency());

    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(numBuffers);
    if (!GetLogicalProcessorInformation(buffer.data(), &bufferSize))
        return static_cast<int>(std::thread::hardware_concurrency());

    int count = 0;
    for (const auto& info : buffer)
        if (info.Relationship == RelationProcessorCore)
            ++count;

    return count > 0 ? count : static_cast<int>(std::thread::hardware_concurrency());

#elif defined(__linux__)
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return static_cast<int>(std::thread::hardware_concurrency());

    char line[256];
    int cpuCores = 0;
    int physicalId = -1;

    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "cpu cores", 9) == 0)
        {
            const char* colon = strchr(line, ':');
            if (colon)
                cpuCores = atoi(colon + 1);
        }
        else if (strncmp(line, "physical id", 11) == 0)
        {
            const char* colon = strchr(line, ':');
            if (colon)
            {
                int id = atoi(colon + 1);
                if (id > physicalId)
                    physicalId = id;
            }
        }
    }
    fclose(f);

    int result = cpuCores * (physicalId + 1);
    return result > 0 ? result : static_cast<int>(std::thread::hardware_concurrency());

#elif defined(__APPLE__)
    int count = 0;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.physicalcpu", &count, &size, nullptr, 0) == 0 && count > 0)
        return count;
    return static_cast<int>(std::thread::hardware_concurrency());

#else
    return static_cast<int>(std::thread::hardware_concurrency());
#endif
}

} // namespace atk
