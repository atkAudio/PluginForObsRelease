#pragma once

#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
#include <atkaudio/midi_obs_mapping.h>

#include <vector>

namespace atk::settings
{
void initialize();
void shutdown();

bool isLoggingEnabled();
void setLoggingEnabled(bool enabled);

std::vector<atk::MidiObsMapping> getMidiObsMappings();
void setMidiObsMappings(const std::vector<atk::MidiObsMapping>& mappings);

atk::MidiClientState getMidiObsInputSubscriptions();
void setMidiObsInputSubscriptions(const atk::MidiClientState& state);
} // namespace atk::settings
