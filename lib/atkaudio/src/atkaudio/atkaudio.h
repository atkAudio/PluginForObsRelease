#pragma once

#include <atomic>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#ifdef BUILDING_DLL
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT __declspec(dllimport)
#endif
#else
#define DLL_EXPORT __attribute__((visibility("default")))
#endif

namespace atk
{

extern "C" DLL_EXPORT void create();
extern "C" DLL_EXPORT void destroy();

} // namespace atk
