#include "obs_monitoring.h"

#include <obs.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace atk::obsMonitoring
{
namespace
{
bool (*getMonitoringEnabled)(const obs_source_t*) = nullptr;
void (*setMonitoringEnabled)(obs_source_t*, bool) = nullptr;
enum obs_monitoring_type (*getMonitoringType)(const obs_source_t*) = nullptr;
void (*setMonitoringType)(obs_source_t*, enum obs_monitoring_type) = nullptr;

#ifdef _WIN32
FARPROC resolveMonitoringSymbol(const char* name)
{
    HMODULE module = GetModuleHandleW(L"obs.dll");
    return module != nullptr ? GetProcAddress(module, name) : nullptr;
}
#else
void* resolveMonitoringSymbol(const char* name)
{
    return dlsym(RTLD_DEFAULT, name);
}
#endif
} // namespace

bool initialize()
{
    getMonitoringEnabled = nullptr;
    setMonitoringEnabled = nullptr;
    getMonitoringType = nullptr;
    setMonitoringType = nullptr;

    // Keep legacy exports out of the import table so newer OBS can remove them.
    if (obs_get_version() < MAKE_SEMANTIC_VERSION(33, 0, 0))
    {
        getMonitoringType =
            reinterpret_cast<decltype(getMonitoringType)>(resolveMonitoringSymbol("obs_source_get_monitoring_type"));
        setMonitoringType =
            reinterpret_cast<decltype(setMonitoringType)>(resolveMonitoringSymbol("obs_source_set_monitoring_type"));
        return getMonitoringType != nullptr && setMonitoringType != nullptr;
    }

    getMonitoringEnabled =
        reinterpret_cast<decltype(getMonitoringEnabled)>(resolveMonitoringSymbol("obs_source_get_monitoring_enabled"));
    setMonitoringEnabled =
        reinterpret_cast<decltype(setMonitoringEnabled)>(resolveMonitoringSymbol("obs_source_set_monitoring_enabled"));
    return getMonitoringEnabled != nullptr && setMonitoringEnabled != nullptr;
}

bool isEnabled(obs_source_t* source)
{
    if (getMonitoringEnabled != nullptr)
        return getMonitoringEnabled(source);

    return getMonitoringType(source) != OBS_MONITORING_TYPE_NONE;
}

void setEnabled(obs_source_t* source, bool enabled)
{
    if (setMonitoringEnabled != nullptr)
        setMonitoringEnabled(source, enabled);
    else
        setMonitoringType(source, enabled ? OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT : OBS_MONITORING_TYPE_NONE);
}

bool isMonitorOnly(obs_source_t* source)
{
    // OBS 33 removed monitor-only mode; monitoring and output routing are independent.
    return getMonitoringEnabled == nullptr && getMonitoringType(source) == OBS_MONITORING_TYPE_MONITOR_ONLY;
}
} // namespace atk::obsMonitoring
