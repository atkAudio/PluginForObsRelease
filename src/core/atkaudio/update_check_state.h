#pragma once

#include <juce_core/juce_core.h>

namespace atk::updates
{
struct CheckState
{
    juce::String skippedBuildTimestamp;
    juce::int64 skippedAtMs = 0;
};

bool ShouldCheckForUpdate(juce::File versionFile, juce::Time now);
CheckState LoadCheckState(juce::File versionFile, juce::Time now);
juce::Result SaveCheckState(juce::File versionFile, CheckState* state, juce::Time checkedAt);
} // namespace atk::updates
