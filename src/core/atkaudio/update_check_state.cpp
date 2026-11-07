#include "update_check_state.h"

#include "update_manifest.h"

#define UPDATE_CHECK_INTERVAL_MS (7LL * 24 * 60 * 60 * 1000)
#define UPDATE_SKIP_EXPIRY_MS (90LL * 24 * 60 * 60 * 1000)

namespace atk::updates
{
bool ShouldCheckForUpdate(juce::File versionFile, juce::Time now)
{
    if (!versionFile.existsAsFile())
        return true;

    juce::int64 elapsedMs = now.toMilliseconds() - versionFile.getLastModificationTime().toMilliseconds();
    return elapsedMs < 0 || elapsedMs >= UPDATE_CHECK_INTERVAL_MS;
}

CheckState LoadCheckState(juce::File versionFile, juce::Time now)
{
    CheckState state;
    juce::String contents = versionFile.loadFileAsString().trim();
    if (IsValidBuildTimestamp(contents))
    {
        state.skippedBuildTimestamp = contents;
        // Legacy files only recorded the last check time, so the original skip date is unavailable.
        state.skippedAtMs = versionFile.getLastModificationTime().toMilliseconds();
    }
    else
    {
        juce::var parsed = juce::JSON::parse(contents);
        juce::DynamicObject* object = parsed.getDynamicObject();
        if (object == nullptr)
            return {};

        juce::var skippedBuild = object->getProperty("skipped_build_timestamp");
        juce::var skippedAt = object->getProperty("skipped_at_ms");
        if (!skippedBuild.isString() || (!skippedAt.isInt64() && !skippedAt.isInt()))
            return {};

        state.skippedBuildTimestamp = skippedBuild.toString();
        state.skippedAtMs = skippedAt;
    }

    if (!IsValidBuildTimestamp(state.skippedBuildTimestamp)
        || state.skippedAtMs <= 0
        || state.skippedAtMs > now.toMilliseconds()
        || now.toMilliseconds() - state.skippedAtMs >= UPDATE_SKIP_EXPIRY_MS)
        return {};

    return state;
}

juce::Result SaveCheckState(juce::File versionFile, CheckState* state, juce::Time checkedAt)
{
    if (versionFile.getFullPathName().isEmpty())
        return juce::Result::fail("settings file path is empty; unable to persist update state");

    juce::Result result = versionFile.getParentDirectory().createDirectory();
    if (result.failed())
        return result;

    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    object->setProperty("skipped_build_timestamp", state->skippedBuildTimestamp);
    object->setProperty("skipped_at_ms", state->skippedAtMs);
    if (!versionFile.replaceWithText(juce::JSON::toString(juce::var(object.get()))))
        return juce::Result::fail("unable to save update state; path=\"" + versionFile.getFullPathName() + "\"");

    if (!versionFile.setLastModificationTime(checkedAt))
        return juce::Result::fail("unable to save update check time; path=\"" + versionFile.getFullPathName() + "\"");

    return juce::Result::ok();
}
} // namespace atk::updates
