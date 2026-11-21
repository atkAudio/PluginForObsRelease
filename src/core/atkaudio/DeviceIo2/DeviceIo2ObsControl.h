#pragma once

#include <string>

namespace atk::deviceIo2ObsControl
{
struct SourceLevelState
{
    float sourceVolume = 1.0f;
    bool sourceMuted = false;
};

bool resolveDesiredBypass(const std::string& parentSourceUuid, bool& desiredBypass);
bool readSourceLevelState(const std::string& parentSourceUuid, SourceLevelState& state);
// Call from the OBS UI thread with obs_frontend_get_transition_duration().
void setTransitionDurationMs(int durationMs);
double getCurrentTransitionFadeSeconds();
} // namespace atk::deviceIo2ObsControl
