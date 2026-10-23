#pragma once

#include <juce_core/juce_core.h>

namespace atk::updates
{
struct Manifest
{
    juce::String id;
    juce::String version;
    juce::String buildTimestamp;
    juce::String manifestUrl;
};

bool IsValidBuildTimestamp(juce::String timestamp);
juce::Result ParseManifest(juce::String json, juce::String expectedId, Manifest* manifest);
bool ShouldOfferUpdate(Manifest* manifest, juce::String localTimestamp, juce::String skippedTimestamp);
} // namespace atk::updates
