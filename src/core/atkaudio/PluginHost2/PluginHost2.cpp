#include "PluginHost2.h"

#include "PluginGraph.h"
#include "Editor/MainHostWindow.h"
#include <atkaudio/Logging.h>
#include <atkaudio/atkAudioModule.h>
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleBridge.h>
#include <obs-module.h>

namespace atk
{
class MidiClient;

class PluginHost2RuntimeAudioCallback final
    : public juce::AudioIODeviceCallback
    , private juce::ChangeListener
{
public:
    PluginHost2RuntimeAudioCallback(
        AudioProcessorGraphMT& graphModel,
        juce::AudioDeviceManager& deviceManagerRef,
        atk::MidiClient& midiClientRef
    )
        : graph(graphModel)
        , deviceManager(deviceManagerRef)
        , midiClient(midiClientRef)
    {
        atk::logging::debug("PluginHost2RuntimeAudioCallback::ctor", "begin");
        updateMidiOutput();
        deviceManager.addChangeListener(this);
        atk::logging::debug("PluginHost2RuntimeAudioCallback::ctor", "ready");
    }

    ~PluginHost2RuntimeAudioCallback() override
    {
        atk::logging::debug("PluginHost2RuntimeAudioCallback::dtor", "begin");
        deviceManager.removeChangeListener(this);
        audioDeviceStopped();

        if (midiOutput != nullptr)
            midiOutput->stopBackgroundThread();

        atk::logging::debug("PluginHost2RuntimeAudioCallback::dtor", "completed");
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        const juce::ScopedLock lock(callbackLock);

        if (device != nullptr) {
            sampleRate = device->getCurrentSampleRate();
            blockSize = device->getCurrentBufferSizeSamples();
        }

        auto& audioGraph = graph;
        audioGraph.setPlayConfigDetails(
            device ? device->getActiveInputChannels().countNumberOfSetBits() : 0,
            device ? device->getActiveOutputChannels().countNumberOfSetBits() : 0,
            sampleRate,
            blockSize
        );

        audioGraph.prepareToPlay(sampleRate, blockSize);
        isPrepared = true;
        loadMeasurer.reset(sampleRate, blockSize);
    }

    void audioDeviceStopped() override
    {
        const juce::ScopedLock lock(callbackLock);

        if (isPrepared) {
            graph.releaseResources();
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
        if (!tryLock.isLocked() || !isPrepared) {
            for (int i = 0; i < numOutputChannels; ++i)
                if (outputChannelData[i] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[i], numSamples);
            return;
        }

        juce::AudioProcessLoadMeasurer::ScopedTimer timer(loadMeasurer, numSamples);

        juce::MidiBuffer midiMessages;
        midiClient.getPendingMidi(midiMessages, numSamples, sampleRate);

        juce::AudioBuffer<float> audioBuffer(outputChannelData, numOutputChannels, numSamples);

        for (int ch = 0; ch < juce::jmin(numInputChannels, numOutputChannels); ++ch)
            if (inputChannelData[ch] != nullptr && outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::copy(
                    outputChannelData[ch],
                    inputChannelData[ch],
                    numSamples
                );

        for (int ch = numInputChannels; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

        graph.processBlock(audioBuffer, midiMessages);
        midiClient.sendMidi(midiMessages);

        if (midiOutput != nullptr)
            midiOutput->sendBlockOfMessagesNow(midiMessages);
    }

    void audioDeviceIOCallback(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples
    )
    {
        juce::AudioIODeviceCallbackContext dummyContext;
        audioDeviceIOCallbackWithContext(
            inputChannelData,
            numInputChannels,
            outputChannelData,
            numOutputChannels,
            numSamples,
            dummyContext
        );
    }

    float getCpuLoad() const
    {
        float currentLoad = static_cast<float>(loadMeasurer.getLoadAsProportion());

        auto now = juce::Time::getMillisecondCounterHiRes();
        if (currentLoad >= peakCpuLoad || now - peakCpuTime > 3000.0) {
            peakCpuLoad = currentLoad;
            peakCpuTime = now;
        }

        return peakCpuLoad;
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        updateMidiOutput();
    }

    void updateMidiOutput()
    {
        auto* defaultMidiOutput = deviceManager.getDefaultMidiOutput();

        if (midiOutput == defaultMidiOutput)
            return;

        if (midiOutput != nullptr)
            midiOutput->stopBackgroundThread();

        midiOutput = defaultMidiOutput;

        if (midiOutput != nullptr)
            midiOutput->startBackgroundThread();
    }

    AudioProcessorGraphMT& graph;
    juce::AudioDeviceManager& deviceManager;
    atk::MidiClient& midiClient;
    juce::CriticalSection callbackLock;
    double sampleRate = 44100.0;
    int blockSize = 512;
    bool isPrepared = false;
    juce::AudioProcessLoadMeasurer loadMeasurer;
    juce::MidiOutput* midiOutput = nullptr;
    mutable float peakCpuLoad = 0.0f;
    mutable double peakCpuTime = 0.0;
};
} // namespace atk

atk::PluginHost2::PluginHost2()
    : audioDeviceManager(std::make_unique<juce::AudioDeviceManager>())
    , mainHostWindow(std::make_unique<MainHostWindow>(*audioDeviceManager))
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

    runtimeAudioCallback = std::make_unique<PluginHost2RuntimeAudioCallback>(
        graphModel->graph,
        *audioDeviceManager,
        moduleDeviceManager->getMidiClient()
    );

    mainHostWindow->setRuntimeCpuLoadProvider([callback = runtimeAudioCallback.get()]() {
        return callback->getCpuLoad();
    });

    audioDeviceManager->addAudioCallback(runtimeAudioCallback.get());

    const bool initialized = moduleDeviceManager->initialize();

    if (initialized) {
        moduleDeviceManager->openOBSDevice();
        atk::logging::info(
            "PluginHost2::ctor",
            "ModuleDeviceManager initialized and OBS device opened"
        );
    } else {
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
    auto* deviceManagerPtr = moduleDeviceManager.release();
    auto* runtimeAudioCallbackPtr = runtimeAudioCallback.release();
    auto* audioDeviceManagerPtr = audioDeviceManager.release();

    atkAudioModule::destroyOnMessageThread(
        [windowPtr,
         graphModelPtr,
         deviceManagerPtr,
         runtimeAudioCallbackPtr,
         audioDeviceManagerPtr]() {
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

    graphModel->clear();

    auto xml = juce::XmlDocument::parse(pendingStateString);
    if (!xml) {
        pendingStateString.clear();
        return;
    }

    auto* savedState = xml->getChildByName("DEVICESETUP");
    auto& deviceManager = *audioDeviceManager;
    if (savedState)
        deviceManager.initialise(256, 256, savedState, true);

    juce::XmlElement* filterGraph = xml->getChildByName("FILTERGRAPH");
    if (filterGraph != nullptr)
        graphModel->restoreFromXml(*filterGraph);

    auto* audioServerElement = xml->getChildByName("AUDIOSERVER");
    if (audioServerElement) {
        auto* audioServer = atk::AudioServer::getInstance();

        atk::logging::debug("PluginHost2::setState", "restoring AudioServer device settings");
        for (auto* deviceElement : audioServerElement->getChildIterator()) {
            if (deviceElement->hasTagName("DEVICE")) {
                juce::String deviceName = deviceElement->getStringAttribute("name");

                if (deviceElement->hasAttribute("sampleRate")) {
                    double sampleRate = deviceElement->getDoubleAttribute("sampleRate");
                    atk::logging::debug(
                        "PluginHost2::setState",
                        "restore sample rate for " + deviceName + " -> " + juce::String(sampleRate)
                    );
                    audioServer->setDeviceSampleRate(deviceName, sampleRate);
                }

                if (deviceElement->hasAttribute("bufferSize")) {
                    int bufferSize = deviceElement->getIntAttribute("bufferSize");
                    atk::logging::debug(
                        "PluginHost2::setState",
                        "restore buffer size for " + deviceName + " -> " + juce::String(bufferSize)
                    );
                    audioServer->setDeviceBufferSize(deviceName, bufferSize);
                }
            }
        }
    }

    auto* midiElement = xml->getChildByName("MIDISTATE");
    if (midiElement != nullptr) {
        atk::MidiClientState midiState;
        midiState.deserialize(midiElement->getStringAttribute("state"));
        moduleDeviceManager->getMidiClient().setSubscriptions(midiState);
    }

    pendingStateString.clear();
}

void atk::PluginHost2::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    moduleDeviceManager->processExternalAudio(buffer, numChannels, numSamples, sampleRate);
}

void atk::PluginHost2::getState(std::string& s)
{
    juce::XmlElement xml("atkAudioPluginHost2State");

    auto state = audioDeviceManager->createStateXml();
    if (state != nullptr)
        xml.addChildElement(state.release());

    auto* audioServerElement = new juce::XmlElement("AUDIOSERVER");
    if (auto* audioServer = atk::AudioServer::getInstance()) {
        // Use only already-open devices to avoid triggering a full
        // scanForDevices() on every save, which stalls the OBS main thread.
        auto openDevices = audioServer->getOpenDeviceNames();

        for (const auto& deviceName : openDevices) {
            double sampleRate = audioServer->getCurrentSampleRate(deviceName);
            int bufferSize = audioServer->getCurrentBufferSize(deviceName);

            if (sampleRate > 0.0 || bufferSize > 0) {
                auto* deviceElement = new juce::XmlElement("DEVICE");
                deviceElement->setAttribute("name", deviceName);
                if (sampleRate > 0.0)
                    deviceElement->setAttribute("sampleRate", sampleRate);
                if (bufferSize > 0)
                    deviceElement->setAttribute("bufferSize", bufferSize);

                audioServerElement->addChildElement(deviceElement);

                atk::logging::debug(
                    "PluginHost2::getState",
                    "saved device settings for "
                        + deviceName
                        + " (sr="
                        + juce::String(sampleRate)
                        + ", bs="
                        + juce::String(bufferSize)
                        + ")"
                );
            }
        }
    }
    xml.addChildElement(audioServerElement);

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

    if (mainHostWindow->graphHolder == nullptr) {
        atk::logging::debug("PluginHost2::getWindowComponent", "attaching graph editor");
        mainHostWindow->attachGraph(*graphModel);
    } else {
        atk::logging::debug("PluginHost2::getWindowComponent", "graph editor already attached");
    }

    return mainHostWindow.get();
}

void atk::PluginHost2::setParentSource(void* parentSource)
{
    auto* source = static_cast<obs_source_t*>(parentSource);
    if (source != nullptr) {
        const char* uuid = obs_source_get_uuid(source);
        mainHostWindow->setParentSourceUuid(uuid ? uuid : "");
    }
}
