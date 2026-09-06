#pragma once

#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <atkaudio/midi_control/midi_control_mapping.h>

#include <vector>

namespace atk::settings
{
void initialize();
void shutdown();

bool isLoggingEnabled();
void setLoggingEnabled(bool enabled);

std::vector<atk::MidiControlMapping> getMidiControlMappings();
void setMidiControlMappings(const std::vector<atk::MidiControlMapping>& mappings);

atk::MidiClientState getMidiControlSubscriptions();
void setMidiControlSubscriptions(const atk::MidiClientState& state);

bool isMidiControlDelayedFeedbackOutputEnabled();
void setMidiControlDelayedFeedbackOutputEnabled(bool enabled);

bool isMidiControlParameterAutoSyncEnabled();
void setMidiControlParameterAutoSyncEnabled(bool enabled);

bool isMidiControlMatchByNameEnabled();
void setMidiControlMatchByNameEnabled(bool enabled);

bool isMidiControlLastTouchedTrackingEnabled();
void setMidiControlLastTouchedTrackingEnabled(bool enabled);

int getMidiControlDelayedFeedbackOutputIdleMs();
void setMidiControlDelayedFeedbackOutputIdleMs(int delayMs);

std::vector<juce::String> getObsFilterLastTouchedHistoryForCollection(const juce::String& collectionId);
void setObsFilterLastTouchedHistoryForCollection(
    const juce::String& collectionId,
    const std::vector<juce::String>& identities
);

std::vector<juce::String> getObsFilterLastTouchedHistory();
void setObsFilterLastTouchedHistory(const std::vector<juce::String>& identities);

std::vector<juce::String> getPluginLastTouchedHistoryForCollection(const juce::String& collectionId);
void setPluginLastTouchedHistoryForCollection(
    const juce::String& collectionId,
    const std::vector<juce::String>& identities
);

std::vector<juce::String> getPluginLastTouchedHistory();
void setPluginLastTouchedHistory(const std::vector<juce::String>& identities);

int getMidiControlLastTouchedPresetScope();
void setMidiControlLastTouchedPresetScope(int scope);

int getMidiControlLastTouchedPresetBank();
void setMidiControlLastTouchedPresetBank(int bank);

int getMidiControlLastTouchedPresetPreset();
void setMidiControlLastTouchedPresetPreset(int preset);

std::vector<int> getMidiControlLastTouchedPresetSlots();
void setMidiControlLastTouchedPresetSlots(const std::vector<int>& slots);

bool shouldRestoreMidiControlActiveContextDock();
void setRestoreMidiControlActiveContextDock(bool enabled);

bool shouldRestoreMidiControlLastTouchedDock();
void setRestoreMidiControlLastTouchedDock(bool enabled);
} // namespace atk::settings
