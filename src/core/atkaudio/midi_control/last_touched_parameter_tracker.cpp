#include "last_touched_parameter_tracker.h"

#include <atkaudio/GlobalSettings.h>
#include <atkaudio/Logging.h>

#include <algorithm>
#include <unordered_set>
#include <cstdlib>

namespace
{
constexpr int kMaxRecentParameters = 32;
constexpr int kPostWriteSuppressionBudget = 4;
constexpr double kPostWriteSuppressionWindowMs = 100.0;
constexpr const char* kDefaultHistoryCollectionId = "default";

[[noreturn]] void failFast(const char* context, const juce::String& message)
{
    atk::logging::error(context, "Fatal: " + message);
    std::abort();
}

juce::String normalizeCollectionId(const juce::String& collectionId)
{
    auto normalized = collectionId.trim();
    if (normalized.isEmpty())
        normalized = kDefaultHistoryCollectionId;

    return normalized;
}
} // namespace

namespace atk
{

class LastTouchedParameterTracker::ProcessorRegistration : private juce::AudioProcessorParameter::Listener
{
public:
    ProcessorRegistration(
        LastTouchedParameterTracker& ownerRef,
        juce::AudioProcessor& processorRef,
        juce::String sourceUuid,
        juce::String filterName
    )
        : owner(ownerRef)
        , processor(processorRef)
        , ownerSourceUuid(std::move(sourceUuid))
        , ownerFilterName(std::move(filterName))
    {
        auto& parameters = processor.getParameters();
        observedParameters.reserve(size_t(parameters.size()));

        for (auto* parameter : parameters)
        {
            if (parameter == nullptr)
                continue;

            observedParameters.push_back(parameter);
            parameter->addListener(this);
        }
    }

    ~ProcessorRegistration() override
    {
        for (auto* parameter : observedParameters)
            if (parameter != nullptr)
                parameter->removeListener(this);
    }

private:
    void parameterValueChanged(int parameterIndex, float newValue) override
    {
        juce::ignoreUnused(newValue);

        if (gestureBackedParameters.find(parameterIndex) != gestureBackedParameters.end())
        {
            // For gesture-aware parameters, allow continuous updates only while a gesture is active.
            if (activeGestureParameters.find(parameterIndex) == activeGestureParameters.end())
                return;

            owner.noteParameterTouched(processor, parameterIndex, ownerSourceUuid, ownerFilterName);
            return;
        }

        auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
        if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
            return;

        owner.noteParameterTouched(processor, parameterIndex, ownerSourceUuid, ownerFilterName);
    }

    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override
    {
        gestureBackedParameters.insert(parameterIndex);

        if (!gestureIsStarting)
        {
            activeGestureParameters.erase(parameterIndex);
            return;
        }

        activeGestureParameters.insert(parameterIndex);

        owner.noteParameterTouched(processor, parameterIndex, ownerSourceUuid, ownerFilterName);
    }

    LastTouchedParameterTracker& owner;
    juce::AudioProcessor& processor;
    std::vector<juce::AudioProcessorParameter*> observedParameters;
    std::unordered_set<int> gestureBackedParameters;
    std::unordered_set<int> activeGestureParameters;
    juce::String ownerSourceUuid;
    juce::String ownerFilterName;
};

LastTouchedParameterTracker& LastTouchedParameterTracker::getInstance()
{
    static LastTouchedParameterTracker instance;
    return instance;
}

void LastTouchedParameterTracker::registerProcessor(
    juce::AudioProcessor& processor,
    juce::String ownerSourceUuid,
    juce::String ownerFilterName
)
{
    const juce::ScopedLock lock(trackerLock);
    ensurePersistenceLoaded();

    auto existing = registrations.find(&processor);
    if (existing != registrations.end())
        return;

    auto sourceUuid = ownerSourceUuid;
    auto filterName = ownerFilterName;

    registrations.emplace(
        &processor,
        std::make_unique<ProcessorRegistration>(*this, processor, std::move(ownerSourceUuid), std::move(ownerFilterName))
    );

    updateProcessorParameterState(processor, sourceUuid, filterName);
}

void LastTouchedParameterTracker::unregisterProcessor(juce::AudioProcessor& processor)
{
    const juce::ScopedLock lock(trackerLock);
    ensurePersistenceLoaded();

    registrations.erase(&processor);
    removeProcessorEntries(processor);
}

void LastTouchedParameterTracker::clear()
{
    const juce::ScopedLock lock(trackerLock);

    registrations.clear();
    parameterStateByIdentity.clear();
    recentParameters.clear();
    internalWriteStateByParameter.clear();
    nextTouchSequence = 0;
    persistenceLoaded = true;
    persistHistoryLocked();
}

bool LastTouchedParameterTracker::getParameterAtOffset(int offset, LastTouchedParameterEntry& entry) const
{
    const juce::ScopedLock lock(trackerLock);

    const_cast<LastTouchedParameterTracker*>(this)->ensurePersistenceLoaded();

    if (offset < 0 || offset >= int(recentParameters.size()))
        return false;

    auto recentEntry = recentParameters[size_t(offset)];
    if (recentEntry.identity.isEmpty())
        failFast("LastTouchedTracker", "Recent plugin history entry has empty identity");

    auto stateIt = parameterStateByIdentity.find(recentEntry.identity.toStdString());
    if (stateIt == parameterStateByIdentity.end())
        return false;

    entry = stateIt->second.entry;
    entry.touchSequence = recentEntry.touchSequence;
    return true;
}

bool LastTouchedParameterTracker::getParameterByIdentity(
    const juce::String& identity,
    LastTouchedParameterEntry& entry
) const
{
    const juce::ScopedLock lock(trackerLock);

    const_cast<LastTouchedParameterTracker*>(this)->ensurePersistenceLoaded();

    auto key = identity.trim().toStdString();
    if (key.empty())
        return false;

    auto stateIt = parameterStateByIdentity.find(key);
    if (stateIt == parameterStateByIdentity.end())
        return false;

    entry = stateIt->second.entry;
    return true;
}

bool LastTouchedParameterTracker::getParameterForOwner(
    const juce::String& ownerSourceUuid,
    const juce::String& ownerFilterName,
    int parameterIndex,
    LastTouchedParameterEntry& entry
) const
{
    if (ownerSourceUuid.isEmpty() || ownerFilterName.isEmpty() || parameterIndex < 0)
        return false;

    auto identity = createIdentity(ownerSourceUuid, ownerFilterName, parameterIndex);
    return getParameterByIdentity(identity, entry);
}

std::vector<LastTouchedParameterEntry> LastTouchedParameterTracker::getParametersForOwner(
    const juce::String& ownerSourceUuid,
    const juce::String& ownerFilterName
) const
{
    std::vector<LastTouchedParameterEntry> entries;

    if (ownerSourceUuid.isEmpty() || ownerFilterName.isEmpty())
        return entries;

    const juce::ScopedLock lock(trackerLock);

    const_cast<LastTouchedParameterTracker*>(this)->ensurePersistenceLoaded();

    for (const auto& stateEntry : parameterStateByIdentity)
    {
        auto candidate = stateEntry.second.entry;
        if (candidate.ownerSourceUuid != ownerSourceUuid || candidate.ownerFilterName != ownerFilterName)
            continue;

        entries.push_back(candidate);
    }

    std::sort(
        entries.begin(),
        entries.end(),
        [](const LastTouchedParameterEntry& left, const LastTouchedParameterEntry& right)
        {
            if (left.parameterIndex == right.parameterIndex)
                return left.identity.compareIgnoreCase(right.identity) < 0;

            return left.parameterIndex < right.parameterIndex;
        }
    );

    entries.erase(
        std::unique(
            entries.begin(),
            entries.end(),
            [](const LastTouchedParameterEntry& left, const LastTouchedParameterEntry& right)
            { return left.parameterIndex == right.parameterIndex && left.identity == right.identity; }
        ),
        entries.end()
    );

    return entries;
}

std::vector<LastTouchedParameterEntry> LastTouchedParameterTracker::getRecentParametersSnapshot() const
{
    const juce::ScopedLock lock(trackerLock);

    const_cast<LastTouchedParameterTracker*>(this)->ensurePersistenceLoaded();

    std::vector<LastTouchedParameterEntry> snapshot;
    snapshot.reserve(recentParameters.size());

    for (const auto& candidate : recentParameters)
    {
        if (candidate.identity.isEmpty())
            failFast("LastTouchedTracker", "Recent plugin history entry has empty identity");

        auto stateIt = parameterStateByIdentity.find(candidate.identity.toStdString());
        if (stateIt == parameterStateByIdentity.end())
            continue;

        auto current = stateIt->second.entry;
        current.touchSequence = candidate.touchSequence;
        snapshot.push_back(current);
    }

    return snapshot;
}

uint64_t LastTouchedParameterTracker::getLatestTouchSequence() const
{
    const juce::ScopedLock lock(trackerLock);
    return nextTouchSequence;
}

int LastTouchedParameterTracker::addTouchListener(std::function<void()> listener)
{
    if (!listener)
        return 0;

    const juce::ScopedLock lock(touchListenersLock);
    auto listenerId = nextTouchListenerId++;
    touchListeners.push_back({listenerId, std::move(listener)});
    return listenerId;
}

void LastTouchedParameterTracker::removeTouchListener(int listenerId)
{
    if (listenerId <= 0)
        return;

    const juce::ScopedLock lock(touchListenersLock);

    touchListeners.erase(
        std::remove_if(
            touchListeners.begin(),
            touchListeners.end(),
            [listenerId](const std::pair<int, std::function<void()>>& entry) { return entry.first == listenerId; }
        ),
        touchListeners.end()
    );
}

void LastTouchedParameterTracker::setHistoryHoldEnabled(bool enabled)
{
    const juce::ScopedLock lock(trackerLock);
    historyHoldEnabled = enabled;
}

bool LastTouchedParameterTracker::isHistoryHoldEnabled() const
{
    const juce::ScopedLock lock(trackerLock);
    return historyHoldEnabled;
}

bool LastTouchedParameterTracker::toggleHistoryHoldEnabled()
{
    const juce::ScopedLock lock(trackerLock);
    historyHoldEnabled = !historyHoldEnabled;
    return historyHoldEnabled;
}

void LastTouchedParameterTracker::setActiveCollectionId(const juce::String& collectionId)
{
    const juce::ScopedLock lock(trackerLock);

    auto normalized = normalizeCollectionId(collectionId);
    if (activeCollectionId == normalized && persistenceLoaded)
        return;

    activeCollectionId = normalized;
    recentParameters.clear();
    nextTouchSequence = 0;
    persistenceLoaded = false;
}

juce::String LastTouchedParameterTracker::getActiveCollectionId() const
{
    const juce::ScopedLock lock(trackerLock);
    return activeCollectionId;
}

void LastTouchedParameterTracker::setPersistenceSuspended(bool suspended)
{
    const juce::ScopedLock lock(trackerLock);
    persistenceSuspended = suspended;
}

void LastTouchedParameterTracker::beginInternalWrite(juce::AudioProcessorParameter& parameter)
{
    const juce::ScopedLock lock(trackerLock);

    auto& state = internalWriteStateByParameter[&parameter];
    ++state.depth;
}

void LastTouchedParameterTracker::endInternalWrite(juce::AudioProcessorParameter& parameter)
{
    const juce::ScopedLock lock(trackerLock);

    auto it = internalWriteStateByParameter.find(&parameter);
    if (it == internalWriteStateByParameter.end())
        return;

    auto& state = it->second;
    if (state.depth > 0)
        --state.depth;

    if (state.depth == 0)
    {
        auto nowMs = juce::Time::getMillisecondCounterHiRes();
        state.postWriteSuppressionBudget = std::max(state.postWriteSuppressionBudget, kPostWriteSuppressionBudget);
        state.suppressionDeadlineMs = nowMs + kPostWriteSuppressionWindowMs;

        if (state.postWriteSuppressionBudget <= 0)
            internalWriteStateByParameter.erase(it);
    }
}

void LastTouchedParameterTracker::noteParameterTouched(
    juce::AudioProcessor& processor,
    int parameterIndex,
    const juce::String& ownerSourceUuid,
    const juce::String& ownerFilterName
)
{
    auto shouldNotify = false;

    {
        const juce::ScopedLock lock(trackerLock);
        ensurePersistenceLoaded();

        if (registrations.find(&processor) == registrations.end())
            return;

        auto& parameters = processor.getParameters();
        if (parameterIndex < 0 || parameterIndex >= parameters.size())
            return;

        auto* parameter = parameters[parameterIndex];
        if (parameter == nullptr)
            return;

        auto identity = createIdentity(ownerSourceUuid, ownerFilterName, parameterIndex);

        auto internalWrite = internalWriteStateByParameter.find(parameter);
        if (internalWrite != internalWriteStateByParameter.end())
        {
            auto& state = internalWrite->second;
            if (state.depth > 0)
                return;

            auto nowMs = juce::Time::getMillisecondCounterHiRes();

            if (state.postWriteSuppressionBudget > 0)
            {
                if (nowMs <= state.suppressionDeadlineMs)
                {
                    --state.postWriteSuppressionBudget;
                    if (state.postWriteSuppressionBudget < 0)
                        state.postWriteSuppressionBudget = 0;
                    return;
                }

                state.postWriteSuppressionBudget = 0;
            }

            // Internal MIDI/controller writes must never become a new "last touched" entry.
            // If we still have a suppression marker for this parameter, swallow this callback.
            // This also handles delayed callbacks that can arrive after the normal suppression window.
            internalWriteStateByParameter.erase(internalWrite);
            return;
        }

        auto existing = std::find_if(
            recentParameters.begin(),
            recentParameters.end(),
            [&identity](const LastTouchedParameterEntry& candidate) { return candidate.identity == identity; }
        );

        if (historyHoldEnabled)
        {
            // Hold freezes slot ordering/membership, but existing held entries remain live.
            if (existing != recentParameters.end())
            {
                existing->touchSequence = ++nextTouchSequence;
                shouldNotify = true;
            }
        }
        else
        {
            if (existing != recentParameters.end())
                recentParameters.erase(existing);

            LastTouchedParameterEntry entry;
            entry.processor = &processor;
            entry.parameter = parameter;
            entry.parameterIndex = parameterIndex;
            entry.ownerSourceUuid = ownerSourceUuid;
            entry.ownerFilterName = ownerFilterName;
            entry.identity = identity;
            entry.touchSequence = ++nextTouchSequence;

            parameterStateByIdentity[identity.toStdString()].entry = entry;

            recentParameters.insert(recentParameters.begin(), entry);

            if (int(recentParameters.size()) > kMaxRecentParameters)
                recentParameters.resize(size_t(kMaxRecentParameters));

            persistHistoryLocked();
            shouldNotify = true;
        }
    }

    if (shouldNotify)
        notifyTouchListeners();
}

void LastTouchedParameterTracker::notifyTouchListeners()
{
    std::vector<std::function<void()>> listenersCopy;

    {
        const juce::ScopedLock lock(touchListenersLock);
        listenersCopy.reserve(touchListeners.size());
        for (const auto& entry : touchListeners)
            listenersCopy.push_back(entry.second);
    }

    for (auto& listener : listenersCopy)
        if (listener)
            listener();
}

void LastTouchedParameterTracker::removeProcessorEntries(juce::AudioProcessor& processor)
{
    for (auto it = parameterStateByIdentity.begin(); it != parameterStateByIdentity.end();)
        if (it->second.entry.processor == &processor)
            it = parameterStateByIdentity.erase(it);
        else
            ++it;

    recentParameters.erase(
        std::remove_if(
            recentParameters.begin(),
            recentParameters.end(),
            [this](const LastTouchedParameterEntry& entry)
            {
                if (entry.identity.isEmpty())
                    failFast("LastTouchedTracker", "Recent plugin history entry has empty identity");

                return parameterStateByIdentity.find(entry.identity.toStdString()) == parameterStateByIdentity.end();
            }
        ),
        recentParameters.end()
    );

    persistHistoryLocked();
}

void LastTouchedParameterTracker::updateProcessorParameterState(
    juce::AudioProcessor& processor,
    const juce::String& ownerSourceUuid,
    const juce::String& ownerFilterName
)
{
    auto& parameters = processor.getParameters();
    for (int parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex)
    {
        auto* parameter = parameters[parameterIndex];
        if (parameter == nullptr)
            continue;

        auto identity = createIdentity(ownerSourceUuid, ownerFilterName, parameterIndex);

        LastTouchedParameterEntry entry;
        entry.processor = &processor;
        entry.parameter = parameter;
        entry.parameterIndex = parameterIndex;
        entry.ownerSourceUuid = ownerSourceUuid;
        entry.ownerFilterName = ownerFilterName;
        entry.identity = identity;

        parameterStateByIdentity[identity.toStdString()].entry = entry;
    }
}

void LastTouchedParameterTracker::ensurePersistenceLoaded()
{
    if (persistenceLoaded)
        return;

    recentParameters.clear();
    nextTouchSequence = 0;

    auto persistedIdentities = settings::getPluginLastTouchedHistoryForCollection(activeCollectionId);
    for (auto& identity : persistedIdentities)
    {
        auto trimmed = identity.trim();
        if (trimmed.isEmpty())
            failFast("LastTouchedTracker", "Persisted plugin history contains empty identity");

        LastTouchedParameterEntry entry;
        entry.identity = trimmed;
        entry.touchSequence = ++nextTouchSequence;
        recentParameters.push_back(entry);

        if (int(recentParameters.size()) >= kMaxRecentParameters)
            break;
    }

    persistenceLoaded = true;
}

void LastTouchedParameterTracker::persistHistoryLocked() const
{
    if (persistenceSuspended)
        return;

    std::vector<juce::String> identities;
    identities.reserve(recentParameters.size());

    for (auto& entry : recentParameters)
        if (entry.identity.isNotEmpty())
            identities.push_back(entry.identity);

    settings::setPluginLastTouchedHistoryForCollection(activeCollectionId, identities);
}

juce::String LastTouchedParameterTracker::createIdentity(
    const juce::String& ownerSourceUuid,
    const juce::String& ownerFilterName,
    int parameterIndex
) const
{
    if (ownerSourceUuid.isEmpty() || ownerFilterName.isEmpty() || parameterIndex < 0)
        failFast("LastTouchedTracker", "Invalid plugin identity parts (owner source/filter/index)");

    return ownerSourceUuid + "::" + ownerFilterName + "::" + juce::String(parameterIndex);
}

} // namespace atk
