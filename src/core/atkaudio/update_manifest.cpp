#include "update_manifest.h"

namespace atk::updates
{
bool IsValidBuildTimestamp(juce::String timestamp)
{
    if (timestamp.length() != 15
        || timestamp[8] != '-'
        || !timestamp.substring(0, 8).containsOnly("0123456789")
        || !timestamp.substring(9).containsOnly("0123456789"))
        return false;

    int year = timestamp.substring(0, 4).getIntValue();
    int month = timestamp.substring(4, 6).getIntValue();
    int day = timestamp.substring(6, 8).getIntValue();
    int hour = timestamp.substring(9, 11).getIntValue();
    int minute = timestamp.substring(11, 13).getIntValue();
    int second = timestamp.substring(13, 15).getIntValue();
    if (year == 0 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59)
        return false;

    int daysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
        daysPerMonth[1] = 29;
    return day <= daysPerMonth[month - 1];
}

juce::Result ParseManifest(juce::String json, juce::String expectedId, Manifest* manifest)
{
    *manifest = {};
    juce::var parsed;
    auto result = juce::JSON::parse(json, parsed);
    if (result.failed())
        return result;
    auto* object = parsed.getDynamicObject();
    if (object == nullptr)
        return juce::Result::fail("manifest must be a JSON object");

    juce::String fields[] = {"id", "version", "build_timestamp", "manifest_url"};
    for (int index = 0; index < 4; ++index)
    {
        auto value = object->getProperty(fields[index]);
        if (!value.isString() || value.toString().trim().isEmpty())
            return juce::Result::fail("missing or invalid manifest field: " + fields[index]);
    }

    Manifest candidate{
        object->getProperty("id").toString(),
        object->getProperty("version").toString(),
        object->getProperty("build_timestamp").toString(),
        object->getProperty("manifest_url").toString()
    };
    if (candidate.id != expectedId)
        return juce::Result::fail("manifest plugin id does not match this plugin");
    if (!IsValidBuildTimestamp(candidate.buildTimestamp))
        return juce::Result::fail("invalid manifest build_timestamp; expected UTC yyyymmdd-hhmmss");
    if (!candidate.manifestUrl.startsWith("https://") || !juce::URL(candidate.manifestUrl).isWellFormed())
        return juce::Result::fail("manifest_url must be an HTTPS URL");

    *manifest = candidate;
    return juce::Result::ok();
}

bool ShouldOfferUpdate(Manifest* manifest, juce::String localTimestamp, juce::String skippedTimestamp)
{
    return IsValidBuildTimestamp(localTimestamp)
        && IsValidBuildTimestamp(manifest->buildTimestamp)
        && manifest->buildTimestamp.compare(localTimestamp) > 0
        && manifest->buildTimestamp != skippedTimestamp;
}
} // namespace atk::updates
