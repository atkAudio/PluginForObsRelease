#include "GlobalSettings.h"

#include <atkaudio/atkaudio.h>
#include <atkaudio/Logging.h>

#include <juce_core/juce_core.h>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstdlib>

namespace
{
constexpr const char* kLoggingEnabledKey = "global.logging.enabled";
constexpr const char* kMidiControlMappingsKey = "global.midi_control.mappings";
constexpr const char* kMidiControlSubscriptionsKey = "global.midi_control.io_subscriptions";
constexpr const char* kMidiControlDelayedFeedbackOutputEnabledKey =
    "global.midi_control.feedback.delayed_output_enabled";
constexpr const char* kMidiControlParameterAutoSyncEnabledKey =
    "global.midi_control.feedback.parameter_auto_sync_enabled";
constexpr const char* kMidiControlMatchByNameEnabledKey = "global.midi_control.match_by_name_enabled";
constexpr const char* kMidiControlLastTouchedTrackingEnabledKey = "global.midi_control.last_touched.enabled";
constexpr const char* kMidiControlDelayedFeedbackOutputIdleMsKey =
    "global.midi_control.feedback.delayed_output_idle_ms";
constexpr const char* kObsFilterLastTouchedHistoryByCollectionKey =
    "global.midi_control.last_touched.obs_filter_history_by_collection";
constexpr const char* kPluginLastTouchedHistoryByCollectionKey =
    "global.midi_control.last_touched.plugin_history_by_collection";
constexpr const char* kMidiControlLastTouchedPresetScopeKey = "global.midi_control.last_touched.save_preset_scope";
constexpr const char* kMidiControlLastTouchedPresetBankKey = "global.midi_control.last_touched.save_preset_bank";
constexpr const char* kMidiControlLastTouchedPresetPresetKey = "global.midi_control.last_touched.save_preset_preset";
constexpr const char* kMidiControlLastTouchedPresetSlotsKey = "global.midi_control.last_touched.save_preset_slots";
constexpr const char* kMidiControlActiveContextDockRestoreKey = "global.midi_control.docks.active_context.restore";
constexpr const char* kMidiControlLastTouchedDockRestoreKey = "global.midi_control.docks.last_touched.restore";
constexpr const char* kDefaultHistoryCollectionKey = "default";
constexpr int kMidiControlDelayedFeedbackOutputDefaultIdleMs = 400;
constexpr int kMidiControlDelayedFeedbackOutputMinIdleMs = 50;
constexpr int kMidiControlDelayedFeedbackOutputMaxIdleMs = 5000;
constexpr int kMidiControlMappingContextMin = 1;
constexpr int kMidiControlMappingContextMax = 8;
constexpr int kMidiControlSaveScopePlugin = 0;
constexpr int kMidiControlSaveScopeVideo = 2;

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
std::vector<atk::MidiControlMapping> g_midiObsMappings;
atk::MidiClientState g_midiObsSubscriptions;
bool g_midiObsDelayedFeedbackOutputEnabled = true;
bool g_midiObsParameterAutoSyncEnabled = true;
bool g_midiObsMatchByNameEnabled = true;
bool g_midiObsLastTouchedTrackingEnabled = false;
int g_midiObsDelayedFeedbackOutputIdleMs = kMidiControlDelayedFeedbackOutputDefaultIdleMs;
std::unordered_map<std::string, std::vector<juce::String>> g_obsFilterLastTouchedHistoryByCollection;
std::unordered_map<std::string, std::vector<juce::String>> g_pluginLastTouchedHistoryByCollection;
int g_midiObsLastTouchedPresetScope = kMidiControlSaveScopePlugin;
int g_midiObsLastTouchedPresetBank = 0;
int g_midiObsLastTouchedPresetPreset = 0;
std::vector<int> g_midiObsLastTouchedPresetSlots;
bool g_restoreMidiControlActiveContextDock = false;
bool g_restoreMidiControlLastTouchedDock = false;

[[noreturn]] void failFast(const char* context, const juce::String& message)
{
    atk::logging::error(context, "Fatal: " + message);
    std::abort();
}

juce::String normalizeHistoryCollectionId(const juce::String& collectionId)
{
    auto normalized = collectionId.trim();
    if (normalized.isEmpty())
        normalized = kDefaultHistoryCollectionKey;

    return normalized;
}

std::unordered_map<std::string, std::vector<juce::String>>
deserializeStringArrayMap(const juce::String& payload, const char* settingKey)
{
    std::unordered_map<std::string, std::vector<juce::String>> byCollection;
    if (payload.trim().isEmpty())
        return byCollection;

    auto parsed = juce::JSON::parse(payload);
    auto* object = parsed.getDynamicObject();
    if (object == nullptr)
        failFast("GlobalSettings", "Invalid JSON object payload for " + juce::String(settingKey));

    for (auto& property : object->getProperties())
    {
        auto key = juce::String(property.name.toString()).trim();
        if (key.isEmpty())
            failFast("GlobalSettings", "Empty collection key in " + juce::String(settingKey));

        auto value = property.value;
        if (!value.isArray())
            failFast("GlobalSettings", "Collection payload is not an array in " + juce::String(settingKey));

        auto* values = value.getArray();
        if (values == nullptr)
            failFast("GlobalSettings", "Collection array values missing in " + juce::String(settingKey));

        std::vector<juce::String> identities;
        identities.reserve(values->size());

        for (auto& identityValue : *values)
        {
            if (!identityValue.isString())
                failFast("GlobalSettings", "Non-string history identity in " + juce::String(settingKey));

            auto text = identityValue.toString().trim();
            if (text.isEmpty())
                failFast("GlobalSettings", "Empty history identity in " + juce::String(settingKey));

            identities.push_back(text);
        }

        byCollection[key.toStdString()] = identities;
    }

    return byCollection;
}

juce::String serializeStringArrayMap(const std::unordered_map<std::string, std::vector<juce::String>>& byCollection)
{
    auto object = juce::DynamicObject::Ptr(new juce::DynamicObject());

    for (auto& entry : byCollection)
    {
        auto key = juce::String(entry.first).trim();
        if (key.isEmpty())
            continue;

        juce::Array<juce::var> identities;
        for (auto& identity : entry.second)
        {
            auto normalized = identity.trim();
            if (normalized.isEmpty())
                continue;

            identities.add(normalized);
        }

        object->setProperty(key, juce::var(identities));
    }

    return juce::JSON::toString(juce::var(object.get()), false);
}

std::vector<int> deserializeIntArray(const juce::String& payload, const char* settingKey)
{
    std::vector<int> items;
    if (payload.trim().isEmpty())
        return items;

    auto parsed = juce::JSON::parse(payload);
    if (!parsed.isArray())
        failFast("GlobalSettings", "Invalid JSON int array payload for " + juce::String(settingKey));

    auto* values = parsed.getArray();
    if (values == nullptr)
        failFast("GlobalSettings", "Missing JSON int array values for " + juce::String(settingKey));

    for (auto& value : *values)
    {
        if (!value.isInt() && !value.isInt64() && !value.isDouble())
            failFast("GlobalSettings", "Non-integer slot index in " + juce::String(settingKey));

        auto slot = int(value);
        if (slot < 1 || slot > 8)
            failFast("GlobalSettings", "Out-of-range slot index in " + juce::String(settingKey));

        items.push_back(slot);
    }

    return items;
}

juce::String serializeIntArray(const std::vector<int>& items)
{
    juce::Array<juce::var> array;
    for (auto slot : items)
    {
        if (slot < 1 || slot > 8)
            continue;

        array.add(slot);
    }

    return juce::JSON::toString(juce::var(array), false);
}

std::unordered_map<std::string, juce::String> deserializeStringMap(const juce::String& payload, const char* settingKey)
{
    std::unordered_map<std::string, juce::String> byKey;
    if (payload.trim().isEmpty())
        return byKey;

    auto parsed = juce::JSON::parse(payload);
    auto* object = parsed.getDynamicObject();
    if (object == nullptr)
        failFast("GlobalSettings", "Invalid JSON object payload for " + juce::String(settingKey));

    for (auto& property : object->getProperties())
    {
        auto key = juce::String(property.name.toString()).trim();
        if (key.isEmpty())
            failFast("GlobalSettings", "Empty key in " + juce::String(settingKey));

        if (!property.value.isString())
            failFast("GlobalSettings", "Non-string value in " + juce::String(settingKey));

        byKey[key.toStdString()] = property.value.toString();
    }

    return byKey;
}

juce::String serializeStringMap(const std::unordered_map<std::string, juce::String>& byKey)
{
    auto object = juce::DynamicObject::Ptr(new juce::DynamicObject());

    for (auto& entry : byKey)
    {
        auto key = juce::String(entry.first).trim();
        if (key.isEmpty())
            continue;

        object->setProperty(key, entry.second);
    }

    return juce::JSON::toString(juce::var(object.get()), false);
}

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

    auto midiObsMappingData = g_settingsFile->getValue(kMidiControlMappingsKey);
    if (!atk::deserializeMidiControlMappings(midiObsMappingData, g_midiObsMappings))
        g_midiObsMappings.clear();

    g_midiObsSubscriptions.deserialize(g_settingsFile->getValue(kMidiControlSubscriptionsKey));
    g_midiObsDelayedFeedbackOutputEnabled =
        g_settingsFile->getBoolValue(kMidiControlDelayedFeedbackOutputEnabledKey, true);
    g_midiObsParameterAutoSyncEnabled = g_settingsFile->getBoolValue(kMidiControlParameterAutoSyncEnabledKey, true);
    g_midiObsMatchByNameEnabled = g_settingsFile->getBoolValue(kMidiControlMatchByNameEnabledKey, true);
    g_midiObsLastTouchedTrackingEnabled =
        g_settingsFile->getBoolValue(kMidiControlLastTouchedTrackingEnabledKey, false);
    g_midiObsDelayedFeedbackOutputIdleMs = juce::jlimit(
        kMidiControlDelayedFeedbackOutputMinIdleMs,
        kMidiControlDelayedFeedbackOutputMaxIdleMs,
        g_settingsFile
            ->getIntValue(kMidiControlDelayedFeedbackOutputIdleMsKey, kMidiControlDelayedFeedbackOutputDefaultIdleMs)
    );
    g_obsFilterLastTouchedHistoryByCollection = deserializeStringArrayMap(
        g_settingsFile->getValue(kObsFilterLastTouchedHistoryByCollectionKey),
        kObsFilterLastTouchedHistoryByCollectionKey
    );
    g_pluginLastTouchedHistoryByCollection = deserializeStringArrayMap(
        g_settingsFile->getValue(kPluginLastTouchedHistoryByCollectionKey),
        kPluginLastTouchedHistoryByCollectionKey
    );
    g_midiObsLastTouchedPresetScope = juce::jlimit(
        kMidiControlSaveScopePlugin,
        kMidiControlSaveScopeVideo,
        g_settingsFile->getIntValue(kMidiControlLastTouchedPresetScopeKey, kMidiControlSaveScopePlugin)
    );
    g_midiObsLastTouchedPresetBank = juce::jlimit(
        0,
        kMidiControlMappingContextMax,
        g_settingsFile->getIntValue(kMidiControlLastTouchedPresetBankKey, 0)
    );
    g_midiObsLastTouchedPresetPreset = juce::jlimit(
        0,
        kMidiControlMappingContextMax,
        g_settingsFile->getIntValue(kMidiControlLastTouchedPresetPresetKey, 0)
    );
    g_midiObsLastTouchedPresetSlots = deserializeIntArray(
        g_settingsFile->getValue(kMidiControlLastTouchedPresetSlotsKey),
        kMidiControlLastTouchedPresetSlotsKey
    );
    g_restoreMidiControlActiveContextDock =
        g_settingsFile->getBoolValue(kMidiControlActiveContextDockRestoreKey, false);
    g_restoreMidiControlLastTouchedDock = g_settingsFile->getBoolValue(kMidiControlLastTouchedDockRestoreKey, false);
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

std::vector<atk::MidiControlMapping> atk::settings::getMidiControlMappings()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsMappings;
}

void atk::settings::setMidiControlMappings(const std::vector<atk::MidiControlMapping>& mappings)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsMappings = mappings;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlMappingsKey, atk::serializeMidiControlMappings(g_midiObsMappings));
        g_settingsFile->saveIfNeeded();
    }
}

atk::MidiClientState atk::settings::getMidiControlSubscriptions()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsSubscriptions;
}

void atk::settings::setMidiControlSubscriptions(const atk::MidiClientState& state)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsSubscriptions = state;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlSubscriptionsKey, g_midiObsSubscriptions.serialize());
        g_settingsFile->saveIfNeeded();
    }
}

bool atk::settings::isMidiControlDelayedFeedbackOutputEnabled()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsDelayedFeedbackOutputEnabled;
}

void atk::settings::setMidiControlDelayedFeedbackOutputEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsDelayedFeedbackOutputEnabled = enabled;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlDelayedFeedbackOutputEnabledKey, g_midiObsDelayedFeedbackOutputEnabled);
        g_settingsFile->saveIfNeeded();
    }
}

bool atk::settings::isMidiControlParameterAutoSyncEnabled()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsParameterAutoSyncEnabled;
}

void atk::settings::setMidiControlParameterAutoSyncEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsParameterAutoSyncEnabled = enabled;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlParameterAutoSyncEnabledKey, g_midiObsParameterAutoSyncEnabled);
        g_settingsFile->saveIfNeeded();
    }
}

bool atk::settings::isMidiControlMatchByNameEnabled()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsMatchByNameEnabled;
}

void atk::settings::setMidiControlMatchByNameEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsMatchByNameEnabled = enabled;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlMatchByNameEnabledKey, g_midiObsMatchByNameEnabled);
        g_settingsFile->saveIfNeeded();
    }
}

bool atk::settings::isMidiControlLastTouchedTrackingEnabled()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsLastTouchedTrackingEnabled;
}

void atk::settings::setMidiControlLastTouchedTrackingEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsLastTouchedTrackingEnabled = enabled;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlLastTouchedTrackingEnabledKey, g_midiObsLastTouchedTrackingEnabled);
        g_settingsFile->saveIfNeeded();
    }
}

int atk::settings::getMidiControlDelayedFeedbackOutputIdleMs()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsDelayedFeedbackOutputIdleMs;
}

void atk::settings::setMidiControlDelayedFeedbackOutputIdleMs(int delayMs)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsDelayedFeedbackOutputIdleMs =
        juce::jlimit(kMidiControlDelayedFeedbackOutputMinIdleMs, kMidiControlDelayedFeedbackOutputMaxIdleMs, delayMs);

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlDelayedFeedbackOutputIdleMsKey, g_midiObsDelayedFeedbackOutputIdleMs);
        g_settingsFile->saveIfNeeded();
    }
}

std::vector<juce::String> atk::settings::getObsFilterLastTouchedHistory()
{
    return getObsFilterLastTouchedHistoryForCollection(kDefaultHistoryCollectionKey);
}

std::vector<juce::String> atk::settings::getObsFilterLastTouchedHistoryForCollection(const juce::String& collectionId)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();

    auto key = normalizeHistoryCollectionId(collectionId).toStdString();
    auto it = g_obsFilterLastTouchedHistoryByCollection.find(key);
    if (it == g_obsFilterLastTouchedHistoryByCollection.end())
        return {};

    return it->second;
}

void atk::settings::setObsFilterLastTouchedHistory(const std::vector<juce::String>& identities)
{
    setObsFilterLastTouchedHistoryForCollection(kDefaultHistoryCollectionKey, identities);
}

void atk::settings::setObsFilterLastTouchedHistoryForCollection(
    const juce::String& collectionId,
    const std::vector<juce::String>& identities
)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    auto key = normalizeHistoryCollectionId(collectionId).toStdString();
    g_obsFilterLastTouchedHistoryByCollection[key] = identities;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(
            kObsFilterLastTouchedHistoryByCollectionKey,
            serializeStringArrayMap(g_obsFilterLastTouchedHistoryByCollection)
        );
        g_settingsFile->saveIfNeeded();
    }
}

std::vector<juce::String> atk::settings::getPluginLastTouchedHistory()
{
    return getPluginLastTouchedHistoryForCollection(kDefaultHistoryCollectionKey);
}

std::vector<juce::String> atk::settings::getPluginLastTouchedHistoryForCollection(const juce::String& collectionId)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();

    auto key = normalizeHistoryCollectionId(collectionId).toStdString();
    auto it = g_pluginLastTouchedHistoryByCollection.find(key);
    if (it == g_pluginLastTouchedHistoryByCollection.end())
        return {};

    return it->second;
}

void atk::settings::setPluginLastTouchedHistory(const std::vector<juce::String>& identities)
{
    setPluginLastTouchedHistoryForCollection(kDefaultHistoryCollectionKey, identities);
}

void atk::settings::setPluginLastTouchedHistoryForCollection(
    const juce::String& collectionId,
    const std::vector<juce::String>& identities
)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    auto key = normalizeHistoryCollectionId(collectionId).toStdString();
    g_pluginLastTouchedHistoryByCollection[key] = identities;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(
            kPluginLastTouchedHistoryByCollectionKey,
            serializeStringArrayMap(g_pluginLastTouchedHistoryByCollection)
        );
        g_settingsFile->saveIfNeeded();
    }
}

int atk::settings::getMidiControlLastTouchedPresetScope()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsLastTouchedPresetScope;
}

void atk::settings::setMidiControlLastTouchedPresetScope(int scope)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsLastTouchedPresetScope = juce::jlimit(kMidiControlSaveScopePlugin, kMidiControlSaveScopeVideo, scope);

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlLastTouchedPresetScopeKey, g_midiObsLastTouchedPresetScope);
        g_settingsFile->saveIfNeeded();
    }
}

int atk::settings::getMidiControlLastTouchedPresetBank()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsLastTouchedPresetBank;
}

void atk::settings::setMidiControlLastTouchedPresetBank(int bank)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsLastTouchedPresetBank = juce::jlimit(0, kMidiControlMappingContextMax, bank);

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlLastTouchedPresetBankKey, g_midiObsLastTouchedPresetBank);
        g_settingsFile->saveIfNeeded();
    }
}

int atk::settings::getMidiControlLastTouchedPresetPreset()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsLastTouchedPresetPreset;
}

void atk::settings::setMidiControlLastTouchedPresetPreset(int preset)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsLastTouchedPresetPreset = juce::jlimit(0, kMidiControlMappingContextMax, preset);

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlLastTouchedPresetPresetKey, g_midiObsLastTouchedPresetPreset);
        g_settingsFile->saveIfNeeded();
    }
}

std::vector<int> atk::settings::getMidiControlLastTouchedPresetSlots()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_midiObsLastTouchedPresetSlots;
}

void atk::settings::setMidiControlLastTouchedPresetSlots(const std::vector<int>& slots)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_midiObsLastTouchedPresetSlots.clear();
    for (auto slot : slots)
    {
        if (slot < kMidiControlMappingContextMin || slot > kMidiControlMappingContextMax)
            continue;

        g_midiObsLastTouchedPresetSlots.push_back(slot);
    }

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(
            kMidiControlLastTouchedPresetSlotsKey,
            serializeIntArray(g_midiObsLastTouchedPresetSlots)
        );
        g_settingsFile->saveIfNeeded();
    }
}

bool atk::settings::shouldRestoreMidiControlActiveContextDock()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_restoreMidiControlActiveContextDock;
}

void atk::settings::setRestoreMidiControlActiveContextDock(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_restoreMidiControlActiveContextDock = enabled;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlActiveContextDockRestoreKey, g_restoreMidiControlActiveContextDock);
        g_settingsFile->saveIfNeeded();
    }
}

bool atk::settings::shouldRestoreMidiControlLastTouchedDock()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    ensureSettingsLoaded();
    return g_restoreMidiControlLastTouchedDock;
}

void atk::settings::setRestoreMidiControlLastTouchedDock(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    g_restoreMidiControlLastTouchedDock = enabled;

    if (g_settingsLifecycleState == SettingsLifecycleState::shutdown)
        return;

    ensureSettingsLoaded();

    if (g_settingsFile != nullptr)
    {
        g_settingsFile->setValue(kMidiControlLastTouchedDockRestoreKey, g_restoreMidiControlLastTouchedDock);
        g_settingsFile->saveIfNeeded();
    }
}
