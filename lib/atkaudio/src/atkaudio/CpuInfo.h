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

// Returns mapping of physical core indices to their primary logical core IDs
// On SMT/HT systems, returns the first logical core of each physical core
inline std::vector<int> getPhysicalCoreMapping() noexcept
{
    std::vector<int> mapping;

#ifdef _WIN32
    DWORD bufferSize = 0;
    GetLogicalProcessorInformation(nullptr, &bufferSize);

    const auto numBuffers = bufferSize / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    if (numBuffers == 0)
    {
        const auto logical = std::thread::hardware_concurrency();
        for (unsigned i = 0; i < logical; ++i)
            mapping.push_back(static_cast<int>(i));
        return mapping;
    }

    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(numBuffers);
    if (!GetLogicalProcessorInformation(buffer.data(), &bufferSize))
    {
        const auto logical = std::thread::hardware_concurrency();
        for (unsigned i = 0; i < logical; ++i)
            mapping.push_back(static_cast<int>(i));
        return mapping;
    }

    for (const auto& info : buffer)
    {
        if (info.Relationship == RelationProcessorCore)
        {
            DWORD_PTR mask = info.ProcessorMask;
            for (int bit = 0; bit < static_cast<int>(sizeof(DWORD_PTR) * 8); ++bit)
            {
                if (mask & (static_cast<DWORD_PTR>(1) << bit))
                {
                    mapping.push_back(bit);
                    break;
                }
            }
        }
    }

#elif defined(__linux__)
    FILE* f = fopen("/sys/devices/system/cpu/present", "r");
    int maxCpu = static_cast<int>(std::thread::hardware_concurrency()) - 1;
    if (f)
    {
        if (fscanf(f, "%*d-%d", &maxCpu) != 1)
            maxCpu = static_cast<int>(std::thread::hardware_concurrency()) - 1;
        fclose(f);
    }

    std::vector<int> coreIds(maxCpu + 1, -1);
    for (int cpu = 0; cpu <= maxCpu; ++cpu)
    {
        char path[256];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        FILE* cf = fopen(path, "r");
        if (cf)
        {
            int coreId = -1;
            if (fscanf(cf, "%d", &coreId) == 1)
                coreIds[cpu] = coreId;
            fclose(cf);
        }
    }

    std::vector<bool> seen(maxCpu + 1, false);
    for (int cpu = 0; cpu <= maxCpu; ++cpu)
    {
        int coreId = coreIds[cpu];
        if (coreId >= 0 && !seen[coreId])
        {
            seen[coreId] = true;
            mapping.push_back(cpu);
        }
    }

#elif defined(__APPLE__)
    const auto logical = std::thread::hardware_concurrency();
    int physicalCount = 0;
    size_t size = sizeof(physicalCount);

    if (sysctlbyname("hw.physicalcpu", &physicalCount, &size, nullptr, 0) == 0 && physicalCount > 0)
    {
        const int stride = static_cast<int>(logical) / physicalCount;
        for (int i = 0; i < physicalCount; ++i)
            mapping.push_back(i * stride);
    }
    else
    {
        for (unsigned i = 0; i < logical; ++i)
            mapping.push_back(static_cast<int>(i));
    }

#else
    const auto logical = std::thread::hardware_concurrency();
    for (unsigned i = 0; i < logical; ++i)
        mapping.push_back(static_cast<int>(i));
#endif

    if (mapping.empty())
    {
        const auto logical = std::thread::hardware_concurrency();
        for (unsigned i = 0; i < logical; ++i)
            mapping.push_back(static_cast<int>(i));
    }

    return mapping;
}

} // namespace atk
