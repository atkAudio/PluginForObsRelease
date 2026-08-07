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
double getCurrentTransitionFadeSeconds();
} // namespace atk::deviceIo2ObsControl
