#include "GlobalSettings.h"

#include <atkaudio/atkaudio.h>

#include <juce_core/juce_core.h>

#include <memory>
#include <mutex>

namespace
{
constexpr const char* kLoggingEnabledKey = "global.logging.enabled";
constexpr const char* kMidiObsMappingsKey = "global.midi_obs.mappings";
constexpr const char* kMidiObsSubscriptionsKey = "global.midi_obs.subscriptions";

enum class SettingsLifecycleState
{
    idle,
    active,
    shutdown,
};

std::mutex g_settingsMutex;
std::unique_ptr<juce::PropertiesFile> g_settingsFile;
SettingsLifecycleState g_settingsLifecycleState = SettingsLifecycleState::idle;
bool g_loggingEnabled = false;
std::vector<atk::MidiObsMapping> g_midiObsMappings;
atk::MidiClientState g_midiObsSubscriptions;

void ensureSettingsLoaded()
{
    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    if (g_settingsFile != nullptr)
        return;

    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = 250;

    g_settingsFile = std::make_unique<juce::PropertiesFile>(atk::getSettingsFile("atkAudio Plugin for OBS"), options);

    g_loggingEnabled = g_settingsFile->getBoolValue(kLoggingEnabledKey, false);

    auto midiObsMappingData = g_settingsFile->getValue(kMidiObsMappingsKey);
    if (!atk::deserializeMidiObsMappings(midiObsMappingData, g_midiObsMappings))
        g_midiObsMappings.clear();

    g_midiObsSubscriptions.deserialize(g_settingsFile->getValue(kMidiObsSubscriptionsKey));
    g_settingsLifecycleState = SettingsLifecycleState::active;
}
} // namespace

void atk::settings::initialize()
{
    const std::lock_guard<std::mutex> lock(g_settingsMutex);

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        g_settingsLifecycleState = SettingsLifecycleState::idle;

    ensureSettingsLoaded();
}

void atk::settings::shutdown()
{
    const std::lock_guard<std::mutex> lock(g_settingsMutex);

    if (g_settingsFile != nullptr)
        g_settingsFile->saveIfNeeded();

    g_settingsFile.reset();
    g_settingsLifecycleState = SettingsLifecycleState::shutdown;
}

bool atk::settings::isLoggingEnabled()
{
    const std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_loggingEnabled;
}

void atk::settings::setLoggingEnabled(bool enabled)
{
    const std::lock_guard<std::mutex> lock(g_settingsMutex);

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
    {
        g_loggingEnabled = enabled;
        return;
    }

    ensureSettingsLoaded();

    g_loggingEnabled = enabled;

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kLoggingEnabledKey, enabled);
        g_settingsFile->saveIfNeeded();
    }
}

std::vector<atk::MidiObsMapping> atk::settings::getMidiObsMappings()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsMappings;
}

void atk::settings::setMidiObsMappings(const std::vector<atk::MidiObsMapping>& mappings)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsMappings = mappings;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiObsMappingsKey, atk::serializeMidiObsMappings(g_midiObsMappings));
        g_settingsFile->saveIfNeeded();
    }
}

atk::MidiClientState atk::settings::getMidiObsInputSubscriptions()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsSubscriptions;
}

void atk::settings::setMidiObsInputSubscriptions(const atk::MidiClientState& state)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsSubscriptions = state;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiObsSubscriptionsKey, g_midiObsSubscriptions.serialize());
        g_settingsFile->saveIfNeeded();
    }
}
