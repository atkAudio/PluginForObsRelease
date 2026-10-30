#include "PluginHost2.h"

#include "PluginGraph.h"
#include "ParentSourceBridge.h"
#include <atkaudio/ObsAudioChannelSet.h>
#include "Editor/MainHostWindow.h"
#include <atkaudio/Logging.h>
#include <atkaudio/atkAudioModule.h>
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleBridge.h>
#include <obs-module.h>

namespace atk
{
class MidiClient;

class PluginHost2RuntimeAudioCallback final : public juce::AudioIODeviceCallback
{
public:
    PluginHost2RuntimeAudioCallback(
        AudioProcessorGraphMT& graphModel,
        atk::MidiClient& midiClientRef,
        ParentSourceBridge& parentBridge,
        PublishedTransport& transportSettingsRef,
        Vst3ChordContext& chordContextRef
    )
        : graph(graphModel)
        , bridge(parentBridge)
        , midiClient(midiClientRef)
        , transportSettings(transportSettingsRef)
        , chordContext(chordContextRef)
    {
        atk::logging::debug("PluginHost2RuntimeAudioCallback::ctor", "ready");
    }

    ~PluginHost2RuntimeAudioCallback() override
    {
        atk::logging::debug("PluginHost2RuntimeAudioCallback::dtor", "begin");
        audioDeviceStopped();
        graph.setPlayHead(nullptr);

        atk::logging::debug("PluginHost2RuntimeAudioCallback::dtor", "completed");
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        const juce::ScopedLock lock(callbackLock);

        if (device != nullptr)
        {
            sampleRate = device->getCurrentSampleRate();
            blockSize = device->getCurrentBufferSizeSamples();
        }

        auto& audioGraph = graph;
        bridge.start(
            device == nullptr                  ? ParentSourceBridge::stopped
            : device->getName() == "OBS Audio" ? ParentSourceBridge::obsAudio
                                               : ParentSourceBridge::hardware,
            getDefaultObsAudioChannelSet().size(),
            blockSize,
            sampleRate
        );
        audioGraph.setPlayConfigDetails(
            device ? device->getActiveInputChannels().countNumberOfSetBits() : 0,
            device ? device->getActiveOutputChannels().countNumberOfSetBits() : 0,
            sampleRate,
            blockSize
        );

        transport.restart(sampleRate);
        transportSettings.generatedMidi.ensureSize(65536);
        transport.beginBlock(transportSettings.get());
        audioGraph.setPlayHead(&transport);
        audioGraph.prepareToPlay(sampleRate, blockSize);
        chordContext.reset();
        midiMessages.ensureSize(MIDI_BUFFER_RESERVE_BYTES);
        isPrepared = true;
        loadMeasurer.reset(sampleRate, blockSize);
    }

    void audioDeviceStopped() override
    {
        const juce::ScopedLock lock(callbackLock);
        bridge.stop();

        if (isPrepared)
        {
            graph.releaseResources();
            chordContext.reset();
            isPrepared = false;
        }
    }

    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context
    ) override
    {
        juce::ignoreUnused(context);

        const juce::ScopedTryLock tryLock(callbackLock);
        if (!tryLock.isLocked() || !isPrepared)
        {
            for (int i = 0; i < numOutputChannels; ++i)
                if (outputChannelData[i] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[i], numSamples);
            return;
        }

        juce::AudioProcessLoadMeasurer::ScopedTimer timer(loadMeasurer, numSamples);
        double blockTimeMs = juce::Time::getMillisecondCounterHiRes();

        midiMessages.clear();
        midiClient.getPendingMidi(midiMessages, numSamples, sampleRate, blockTimeMs);

        juce::AudioBuffer<float> audioBuffer(outputChannelData, numOutputChannels, numSamples);

        for (int ch = 0; ch < juce::jmin(numInputChannels, numOutputChannels); ++ch)
            if (inputChannelData[ch] != nullptr && outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::copy(outputChannelData[ch], inputChannelData[ch], numSamples);

        for (int ch = numInputChannels; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

        transport.beginBlock(transportSettings, numSamples);
        chordContext.beginBlock();
        graph.processBlock(audioBuffer, midiMessages);
        transport.endBlock(numSamples);
        midiClient.sendMidi(midiMessages);
    }

    float getCpuLoad() const
    {
        float currentLoad = static_cast<float>(loadMeasurer.getLoadAsProportion());

        auto now = juce::Time::getMillisecondCounterHiRes();
        if (currentLoad >= peakCpuLoad || now - peakCpuTime > 3000.0)
        {
            peakCpuLoad = currentLoad;
            peakCpuTime = now;
        }

        return peakCpuLoad;
    }

private:
    AudioProcessorGraphMT& graph;
    ParentSourceBridge& bridge;
    atk::MidiClient& midiClient;
    PublishedTransport& transportSettings;
    Vst3ChordContext& chordContext;
    HostTransport transport;
    juce::CriticalSection callbackLock;
    double sampleRate = 44100.0;
    int blockSize = 512;
    bool isPrepared = false;
    juce::AudioProcessLoadMeasurer loadMeasurer;
    static constexpr int MIDI_BUFFER_RESERVE_BYTES = 4096;
    juce::MidiBuffer midiMessages; // Audio thread only; reused so incoming MIDI doesn't allocate per block
    mutable float peakCpuLoad = 0.0f;
    mutable double peakCpuTime = 0.0;
};
} // namespace atk

atk::PluginHost2::PluginHost2()
    : audioDeviceManager(std::make_unique<juce::AudioDeviceManager>())
    , mainHostWindow(std::make_unique<MainHostWindow>(*audioDeviceManager))
    , parentSourceBridge(std::make_unique<ParentSourceBridge>())
    , moduleDeviceManager(
          std::make_unique<atk::ModuleDeviceManager>(
              std::make_unique<atk::ModuleAudioIODeviceType>("PluginHost2 Audio"),
              *audioDeviceManager
          )
      )
{
    atk::logging::info("PluginHost2::ctor", "begin");

    mainHostWindow->setExternalMidiClient(moduleDeviceManager->getMidiClient());

    graphModel = std::make_unique<PluginGraph>(
        *mainHostWindow,
        mainHostWindow->getFormatManager(),
        mainHostWindow->getKnownPluginList()
    );
    graphModel->setParentSourceBridge(parentSourceBridge.get());

    runtimeAudioCallback = std::make_unique<PluginHost2RuntimeAudioCallback>(
        graphModel->graph,
        moduleDeviceManager->getMidiClient(),
        *parentSourceBridge,
        graphModel->getPublishedTransport(),
        graphModel->getVst3ChordContext()
    );

    mainHostWindow->setRuntimeCpuLoadProvider([callback = runtimeAudioCallback.get()]()
                                              { return callback->getCpuLoad(); });

    audioDeviceManager->addAudioCallback(runtimeAudioCallback.get());

    const bool initialized = moduleDeviceManager->initialize();

    if (initialized)
    {
        moduleDeviceManager->openOBSDevice();
        atk::logging::info("PluginHost2::ctor", "ModuleDeviceManager initialized and OBS device opened");
    }
    else
    {
        atk::logging::warning("PluginHost2::ctor", "ModuleDeviceManager initialize failed");
    }

    atk::logging::info("PluginHost2::ctor", "completed");
}

atk::PluginHost2::~PluginHost2()
{
    atk::logging::info("PluginHost2::dtor", "begin");

    cancelPendingUpdate();

    auto* windowPtr = mainHostWindow.release();
    auto* graphModelPtr = graphModel.release();
    auto* parentBridgePtr = parentSourceBridge.release();
    auto* deviceManagerPtr = moduleDeviceManager.release();
    auto* runtimeAudioCallbackPtr = runtimeAudioCallback.release();
    auto* audioDeviceManagerPtr = audioDeviceManager.release();

    atkAudioModule::destroyOnMessageThread(
        [windowPtr, graphModelPtr, parentBridgePtr, deviceManagerPtr, runtimeAudioCallbackPtr, audioDeviceManagerPtr]()
        {
            atk::logging::debug("PluginHost2::dtor", "message-thread teardown begin");

            // Explicitly destroy the full editor shell first on the message
            // thread.
            windowPtr->setRuntimeCpuLoadProvider({});
            delete windowPtr;

            audioDeviceManagerPtr->removeAudioCallback(runtimeAudioCallbackPtr);

            delete runtimeAudioCallbackPtr;

            audioDeviceManagerPtr->closeAudioDevice();

            deviceManagerPtr->cleanup();
            delete deviceManagerPtr;

            delete graphModelPtr;
            delete parentBridgePtr;
            delete audioDeviceManagerPtr;

            atk::logging::debug("PluginHost2::dtor", "message-thread teardown completed");
        },
        0
    );
}

void atk::PluginHost2::handleAsyncUpdate()
{
    if (pendingStateString.isEmpty())
        return;

    auto xml = juce::XmlDocument::parse(pendingStateString);
    if (!xml)
    {
        pendingStateString.clear();
        return;
    }

    auto* savedState = xml->getChildByName("DEVICESETUP");
    if (savedState)
        moduleDeviceManager->restoreDeviceState(*savedState);

    juce::XmlElement* filterGraph = xml->getChildByName("FILTERGRAPH");
    if (filterGraph != nullptr)
        graphModel->restoreFromXml(*filterGraph);

    auto* midiElement = xml->getChildByName("MIDISTATE");
    if (midiElement != nullptr)
    {
        atk::MidiClientState midiState;
        midiState.deserialize(midiElement->getStringAttribute("state"));
        moduleDeviceManager->getMidiClient().setSubscriptions(midiState);
    }

    pendingStateString.clear();
}

void atk::PluginHost2::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    if (!parentSourceBridge->processParent(buffer, numChannels, numSamples, sampleRate))
        moduleDeviceManager->processExternalAudio(buffer, numChannels, numSamples, sampleRate);
}

void atk::PluginHost2::getState(std::string& s)
{
    juce::XmlElement xml("atkAudioPluginHost2State");

    auto state = moduleDeviceManager->createDeviceStateXml();
    if (state != nullptr)
        xml.addChildElement(state.release());

    if (auto filterGraph = graphModel->createXml())
        xml.addChildElement(filterGraph.release());

    auto midiState = moduleDeviceManager->getMidiClient().getSubscriptions();
    auto* midiElement = new juce::XmlElement("MIDISTATE");
    midiElement->setAttribute("state", midiState.serialize());
    xml.addChildElement(midiElement);

    s = xml.toString().toStdString();
}

void atk::PluginHost2::setState(std::string& s)
{
    if (s.empty())
        return;

    pendingStateString = juce::String(s);
    triggerAsyncUpdate();
}

juce::Component* atk::PluginHost2::getWindowComponent()
{
    atk::logging::debug("PluginHost2::getWindowComponent", "called");

    if (mainHostWindow->graphHolder == nullptr)
    {
        atk::logging::debug("PluginHost2::getWindowComponent", "attaching graph editor");
        mainHostWindow->attachGraph(*graphModel);
    }
    else
    {
        atk::logging::debug("PluginHost2::getWindowComponent", "graph editor already attached");
    }

    return mainHostWindow.get();
}

void atk::PluginHost2::setParentSource(void* parentSource)
{
    if (!mainHostWindow)
        return;

    auto* source = static_cast<obs_source_t*>(parentSource);
    const char* uuid = source != nullptr ? obs_source_get_uuid(source) : nullptr;
    mainHostWindow->setParentSourceUuid(uuid ? uuid : "");

    if (graphModel)
        graphModel->refreshLastTouchedTrackerOwnership();
}

void atk::PluginHost2::setOwnerFilterName(const std::string& filterName)
{
    if (!mainHostWindow)
        return;

    mainHostWindow->setOwnerFilterName(filterName);

    if (graphModel)
        graphModel->refreshLastTouchedTrackerOwnership();
}
