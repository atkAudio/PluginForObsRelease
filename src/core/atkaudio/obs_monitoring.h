#pragma once

typedef struct obs_source obs_source_t;

namespace atk::obsMonitoring
{
bool initialize();
bool isEnabled(obs_source_t* source);
void setEnabled(obs_source_t* source, bool enabled);
bool isMonitorOnly(obs_source_t* source);
} // namespace atk::obsMonitoring
