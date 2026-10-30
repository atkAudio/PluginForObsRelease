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
} // namespace

bool initialize()
{
    if (obs_get_version() < MAKE_SEMANTIC_VERSION(33, 0, 0))
        return true;

    // Resolve at module load so OBS 31 SDK builds also use the new API on OBS 33.
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(L"obs.dll");
    getMonitoringEnabled =
        reinterpret_cast<decltype(getMonitoringEnabled)>(GetProcAddress(module, "obs_source_get_monitoring_enabled"));
    setMonitoringEnabled =
        reinterpret_cast<decltype(setMonitoringEnabled)>(GetProcAddress(module, "obs_source_set_monitoring_enabled"));
#else
    getMonitoringEnabled =
        reinterpret_cast<decltype(getMonitoringEnabled)>(dlsym(RTLD_DEFAULT, "obs_source_get_monitoring_enabled"));
    setMonitoringEnabled =
        reinterpret_cast<decltype(setMonitoringEnabled)>(dlsym(RTLD_DEFAULT, "obs_source_set_monitoring_enabled"));
#endif
    return getMonitoringEnabled != nullptr && setMonitoringEnabled != nullptr;
}

bool isEnabled(obs_source_t* source)
{
    if (getMonitoringEnabled != nullptr)
        return getMonitoringEnabled(source);

    return obs_source_get_monitoring_type(source) != OBS_MONITORING_TYPE_NONE;
}

void setEnabled(obs_source_t* source, bool enabled)
{
    if (setMonitoringEnabled != nullptr)
        setMonitoringEnabled(source, enabled);
    else
        obs_source_set_monitoring_type(
            source,
            enabled ? OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT : OBS_MONITORING_TYPE_NONE
        );
}

bool isMonitorOnly(obs_source_t* source)
{
    // OBS 33 removed monitor-only mode; monitoring and output routing are independent.
    return getMonitoringEnabled == nullptr
        && obs_source_get_monitoring_type(source) == OBS_MONITORING_TYPE_MONITOR_ONLY;
}
} // namespace atk::obsMonitoring
