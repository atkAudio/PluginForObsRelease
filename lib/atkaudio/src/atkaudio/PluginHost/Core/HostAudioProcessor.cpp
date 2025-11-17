#include "HostAudioProcessor.h"
#include "../UI/HostEditorWindow.h"

using namespace juce;
using juce::NullCheckedInvocation;

//==============================================================================
// AtkAudioPlayHead implementation
//==============================================================================
juce::Optional<juce::AudioPlayHead::PositionInfo> HostAudioProcessorImpl::AtkAudioPlayHead::getPosition() const
{
    return positionInfo;
}

//==============================================================================
// HostAudioProcessorImpl implementation
//==============================================================================
AudioChannelSet HostAudioProcessorImpl::getChannelSetForCount(int numChannels)
{
    if (numChannels <= 0 || numChannels == 2)
        return AudioChannelSet::stereo();
    if (numChannels == 1)
        return AudioChannelSet::mono();

    // For common surround formats, use the named sets
    switch (numChannels)
    {
    case 3:
        return AudioChannelSet::createLCR();
    case 4:
        return AudioChannelSet::quadraphonic();
    case 5:
        return AudioChannelSet::create5point0();
    case 6:
        return AudioChannelSet::create5point1();
    case 7:
        return AudioChannelSet::create7point0();
    case 8:
        return AudioChannelSet::create7point1();
    default:
        return AudioChannelSet::discreteChannels(numChannels);
    }
}

HostAudioProcessorImpl::HostAudioProcessorImpl(int numChannels)
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", getChannelSetForCount(numChannels), true)
              .withOutput("Output", getChannelSetForCount(numChannels), true)
              .withInput("Sidechain", getChannelSetForCount(numChannels), false)
      )
{
    appProperties.setStorageParameters(
        [&]
        {
            PropertiesFile::Options opt;
            opt.applicationName = getName();
            opt.commonToAllUsers = false;
            opt.doNotSave = false;
            opt.filenameSuffix = "settings";
            opt.ignoreCaseOfKeyNames = false;
            opt.storageFormat = PropertiesFile::StorageFormat::storeAsXML;
            opt.osxLibrarySubFolder = "Application Support";
            opt.folderName = "atkAudio Plugin";
            opt.processLock = &appPropertiesLock;
            return opt;
        }()
    );

    pluginFormatManager.addDefaultFormats();

    if (auto savedPluginList = appProperties.getUserSettings()->getXmlValue("pluginList"))
        pluginList.recreateFromXml(*savedPluginList);

    MessageManagerLock lock;
    pluginList.addChangeListener(this);

    // Initialize default diagonal OBS routing (all OBS channels enabled)
    routingMatrix.initializeDefaultMapping(numChannels);

    DBG("[MIDI_SRV] PluginHost created with MidiClient");
}

HostAudioProcessorImpl::~HostAudioProcessorImpl()
{
    DBG("[MIDI_SRV] PluginHost destroying, MidiClient will auto-unregister");
}

bool HostAudioProcessorImpl::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& mainOutput = layouts.getMainOutputChannelSet();
    const auto& mainInput = layouts.getMainInputChannelSet();

    if (!mainInput.isDisabled() && mainInput != mainOutput)
        return false;

    if (mainOutput.size() > 8)
        return false;

    return true;
}

void HostAudioProcessorImpl::prepareToPlay(double sr, int bs)
{
    const ScopedLock sl(innerMutex);

    active = true;

    if (inner != nullptr)
    {
        inner->setRateAndBufferSizeDetails(sr, bs);
        inner->prepareToPlay(sr, bs);
    }

    // Pre-allocate buffers to avoid allocation on audio thread
    // Use generous sizes to handle most scenarios without reallocation
    const int maxChannels = 32;      // Support up to 32 channels
    const int maxSamples = bs * 2;   // Double buffer size for safety
    const int maxSubscriptions = 16; // Support up to 16 device subscriptions

    // Pre-allocate internal processing buffer
    if (internalBuffer.getNumChannels() < maxChannels || internalBuffer.getNumSamples() < maxSamples)
        internalBuffer.setSize(maxChannels, maxSamples, false, false, true);

    // Pre-allocate device I/O buffers
    if (deviceInputBuffer.getNumChannels() < maxSubscriptions || deviceInputBuffer.getNumSamples() < maxSamples)
        deviceInputBuffer.setSize(maxSubscriptions, maxSamples, false, false, true);

    if (deviceOutputBuffer.getNumChannels() < maxSubscriptions || deviceOutputBuffer.getNumSamples() < maxSamples)
        deviceOutputBuffer.setSize(maxSubscriptions, maxSamples, false, false, true);
}

void HostAudioProcessorImpl::releaseResources()
{
    const ScopedLock sl(innerMutex);

    active = false;

    if (inner != nullptr)
    {
        // Safety check: Don't call into JUCE plugin code if MessageManager is gone
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
            inner->releaseResources();
    }
}

void HostAudioProcessorImpl::reset()
{
    const ScopedLock sl(innerMutex);

    if (inner != nullptr)
        inner->reset();
}

void HostAudioProcessorImpl::processBlock(AudioBuffer<float>& buffer, MidiBuffer& midiBuffer)
{
    jassert(!isUsingDoublePrecision());
    if (inner == nullptr)
        return;

    AudioBuffer<float> tempBuffer;

    // Check if plugin has any output channels (input-only plugins shouldn't process)
    int numOutputChannels = 0;
    for (int i = 0; i < inner->getBusCount(false); ++i)
        numOutputChannels += inner->getChannelCountOfBus(false, i);

    if (numOutputChannels == 0)
        return;

    atkPlayHead.positionInfo.setIsPlaying(true);
    atkPlayHead.positionInfo.setBpm(120.0);
    auto pos =
        atkPlayHead.positionInfo.getTimeInSamples().hasValue() ? *atkPlayHead.positionInfo.getTimeInSamples() : 0;
    atkPlayHead.positionInfo.setTimeInSamples(pos + buffer.getNumSamples());
    inner->setPlayHead(&atkPlayHead);

    // Get current subscriptions
    auto clientState = audioClient.getSubscriptions();
    int numInputSubs = (int)clientState.inputSubscriptions.size();
    int numOutputSubs = (int)clientState.outputSubscriptions.size();

    // Ensure internal buffer has enough channels for plugin
    int pluginChannels = buffer.getNumChannels();
    if (internalBuffer.getNumChannels() < pluginChannels || internalBuffer.getNumSamples() < buffer.getNumSamples())
        internalBuffer.setSize(pluginChannels, buffer.getNumSamples(), false, false, true);

    // Ensure device buffers have enough channels
    if (deviceInputBuffer.getNumChannels() < numInputSubs || deviceInputBuffer.getNumSamples() < buffer.getNumSamples())
        deviceInputBuffer.setSize(std::max(numInputSubs, 1), buffer.getNumSamples(), false, false, true);

    if (deviceOutputBuffer.getNumChannels() < numOutputSubs
        || deviceOutputBuffer.getNumSamples() < buffer.getNumSamples())
        deviceOutputBuffer.setSize(std::max(numOutputSubs, 1), buffer.getNumSamples(), false, false, true);

    // Pull all subscribed device inputs (one channel per subscription)
    audioClient.pullSubscribedInputs(deviceInputBuffer, buffer.getNumSamples(), getSampleRate());

    // Apply INPUT routing matrix using ChannelRoutingMatrix
    // Routes (OBS channels + device inputs) → internal buffer (plugin input)
    routingMatrix.applyInputRouting(
        buffer.getArrayOfWritePointers(), // OBS buffer (source)
        deviceInputBuffer,                // Device inputs (source)
        internalBuffer,                   // Internal buffer (target for plugin)
        buffer.getNumChannels(),          // Number of OBS channels
        buffer.getNumSamples(),           // Number of samples
        numInputSubs                      // Number of device input subscriptions
    );

    // Get MIDI from MidiClient
    midiClient.getPendingMidi(midiBuffer, buffer.getNumSamples(), getSampleRate());

    // Save a copy of input MIDI before plugin processes it
    MidiBuffer inputMidiCopy;
    inputMidiCopy.addEvents(midiBuffer, 0, buffer.getNumSamples(), 0);

    // Pass the internal buffer to the plugin
    tempBuffer.setDataToReferTo(
        internalBuffer.getArrayOfWritePointers(),
        internalBuffer.getNumChannels(),
        buffer.getNumSamples()
    );
    inner->processBlock(tempBuffer, midiBuffer);

    // Apply OUTPUT routing matrix using ChannelRoutingMatrix
    // Routes internal buffer (plugin output) → (OBS channels + device outputs)
    routingMatrix.applyOutputRouting(
        internalBuffer,                   // Internal buffer (source from plugin)
        buffer.getArrayOfWritePointers(), // OBS buffer (target)
        deviceOutputBuffer,               // Device outputs (target)
        buffer.getNumChannels(),          // Number of OBS channels
        buffer.getNumSamples(),           // Number of samples
        numOutputSubs                     // Number of device output subscriptions
    );

    // Push all device outputs (one channel per subscription)
    audioClient.pushSubscribedOutputs(deviceOutputBuffer, buffer.getNumSamples(), getSampleRate());

    // Determine which MIDI to send to outputs
    MidiBuffer* outputMidiToSend = nullptr;
    if (inner->isMidiEffect())
        outputMidiToSend = &midiBuffer;
    else
        outputMidiToSend = &inputMidiCopy;

    // Send MIDI to physical outputs if we have output subscriptions
    if (!outputMidiToSend->isEmpty())
    {
        auto clientState = midiClient.getSubscriptions();
        if (!clientState.subscribedOutputDevices.isEmpty())
            midiClient.sendMidi(*outputMidiToSend);
    }
}

void HostAudioProcessorImpl::processBlock(AudioBuffer<double>&, MidiBuffer&)
{
    jassert(isUsingDoublePrecision());
}

bool HostAudioProcessorImpl::hasEditor() const
{
    return false;
}

AudioProcessorEditor* HostAudioProcessorImpl::createEditor()
{
    return nullptr;
}

const String HostAudioProcessorImpl::getName() const
{
    return "atkAudio PluginHost";
}

bool HostAudioProcessorImpl::acceptsMidi() const
{
    return true;
}

bool HostAudioProcessorImpl::producesMidi() const
{
    return true;
}

double HostAudioProcessorImpl::getTailLengthSeconds() const
{
    return 0.0;
}

int HostAudioProcessorImpl::getNumPrograms()
{
    return 0;
}

int HostAudioProcessorImpl::getCurrentProgram()
{
    return 0;
}

void HostAudioProcessorImpl::setCurrentProgram(int)
{
}

const String HostAudioProcessorImpl::getProgramName(int)
{
    return "None";
}

void HostAudioProcessorImpl::changeProgramName(int, const String&)
{
}

void HostAudioProcessorImpl::setInputChannelMapping(const std::vector<std::vector<bool>>& mapping)
{
    routingMatrix.setInputMapping(mapping);
}

std::vector<std::vector<bool>> HostAudioProcessorImpl::getInputChannelMapping() const
{
    return routingMatrix.getInputMapping();
}

void HostAudioProcessorImpl::setOutputChannelMapping(const std::vector<std::vector<bool>>& mapping)
{
    routingMatrix.setOutputMapping(mapping);
}

std::vector<std::vector<bool>> HostAudioProcessorImpl::getOutputChannelMapping() const
{
    return routingMatrix.getOutputMapping();
}

bool HostAudioProcessorImpl::isSidechainEnabled() const
{
    return sidechainEnabled.load();
}

void HostAudioProcessorImpl::setSidechainEnabled(bool enabled)
{
    sidechainEnabled.store(enabled);
}

void HostAudioProcessorImpl::getStateInformation(MemoryBlock& destData)
{
    const ScopedLock sl(innerMutex);

    XmlElement xml("state");

    // Save Audio subscriptions
    auto audioState = audioClient.getSubscriptions();
    auto audioStateStr = audioState.serialize();
    DBG("HostAudioProcessor: Saving audio state: " << audioStateStr);
    xml.setAttribute("audioClientState", audioStateStr);

    // Save MIDI subscriptions
    auto midiState = midiClient.getSubscriptions();
    xml.setAttribute("midiClientState", midiState.serialize());

    // Save channel mappings
    {
        auto inputMapping = routingMatrix.getInputMapping();
        auto outputMapping = routingMatrix.getOutputMapping();

        if (!inputMapping.empty())
        {
            String inputMappingStr;
            for (size_t obsChannel = 0; obsChannel < inputMapping.size(); ++obsChannel)
            {
                for (size_t pluginChannel = 0; pluginChannel < inputMapping[obsChannel].size(); ++pluginChannel)
                {
                    if (inputMapping[obsChannel][pluginChannel])
                    {
                        if (inputMappingStr.isNotEmpty())
                            inputMappingStr += ";";
                        inputMappingStr += String(obsChannel) + "," + String(pluginChannel);
                    }
                }
            }
            xml.setAttribute("inputChannelMapping", inputMappingStr);
        }

        if (!outputMapping.empty())
        {
            String outputMappingStr;
            for (size_t pluginChannel = 0; pluginChannel < outputMapping.size(); ++pluginChannel)
            {
                for (size_t obsChannel = 0; obsChannel < outputMapping[pluginChannel].size(); ++obsChannel)
                {
                    if (outputMapping[pluginChannel][obsChannel])
                    {
                        if (outputMappingStr.isNotEmpty())
                            outputMappingStr += ";";
                        outputMappingStr += String(pluginChannel) + "," + String(obsChannel);
                    }
                }
            }
            xml.setAttribute("outputChannelMapping", outputMappingStr);
        }
    }

    if (inner != nullptr)
    {
        xml.setAttribute(editorStyleTag, (int)editorStyle);

        auto pd = inner->getPluginDescription();

        // Ensure we always save the .vst3 bundle path, not the internal .so path
        if (pd.pluginFormatName == "VST3" && pd.fileOrIdentifier.contains("/Contents/"))
            pd.fileOrIdentifier = pd.fileOrIdentifier.upToLastOccurrenceOf(".vst3", true, false);

        xml.addChildElement(pd.createXml().release());
        xml.addChildElement(
            [this]
            {
                MemoryBlock innerState;
                inner->getStateInformation(innerState);

                auto stateNode = std::make_unique<XmlElement>(innerStateTag);
                stateNode->addTextElement(innerState.toBase64Encoding());
                return stateNode.release();
            }()
        );
    }

    const auto text = xml.toString();
    destData.replaceAll(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

void HostAudioProcessorImpl::setStateInformation(const void* data, int sizeInBytes)
{
    const ScopedLock sl(innerMutex);

    auto xml = XmlDocument::parse(String(CharPointer_UTF8(static_cast<const char*>(data)), (size_t)sizeInBytes));

    // Restore Audio subscriptions immediately
    if (xml->hasAttribute("audioClientState"))
    {
        auto audioStateStr = xml->getStringAttribute("audioClientState");
        DBG("HostAudioProcessor: Restoring audio state: " << audioStateStr);
        atk::AudioClientState audioState;
        audioState.deserialize(audioStateStr);
        audioClient.setSubscriptions(audioState);
    }

    // Restore MIDI subscriptions immediately
    if (xml->hasAttribute("midiClientState"))
    {
        atk::MidiClientState midiState;
        midiState.deserialize(xml->getStringAttribute("midiClientState"));
        midiClient.setSubscriptions(midiState);
    }

    // Restore channel mappings
    {
        auto inputMapping = routingMatrix.getInputMapping();
        auto outputMapping = routingMatrix.getOutputMapping();

        // Restore input mapping
        if (xml->hasAttribute("inputChannelMapping"))
        {
            auto inputMappingStr = xml->getStringAttribute("inputChannelMapping");
            auto tokens = StringArray::fromTokens(inputMappingStr, ";", "");

            // Clear existing mappings
            for (auto& row : inputMapping)
                std::fill(row.begin(), row.end(), false);

            // Restore mappings
            for (const auto& token : tokens)
            {
                auto parts = StringArray::fromTokens(token, ",", "");
                if (parts.size() == 2)
                {
                    int obsChannel = parts[0].getIntValue();
                    int pluginChannel = parts[1].getIntValue();
                    if (obsChannel >= 0
                        && obsChannel < (int)inputMapping.size()
                        && pluginChannel >= 0
                        && pluginChannel < (int)inputMapping[obsChannel].size())
                    {
                        inputMapping[obsChannel][pluginChannel] = true;
                    }
                }
            }

            routingMatrix.setInputMapping(inputMapping);
        }

        // Restore output mapping
        if (xml->hasAttribute("outputChannelMapping"))
        {
            auto outputMappingStr = xml->getStringAttribute("outputChannelMapping");
            auto tokens = StringArray::fromTokens(outputMappingStr, ";", "");

            // Clear existing mappings
            for (auto& row : outputMapping)
                std::fill(row.begin(), row.end(), false);

            // Restore mappings
            for (const auto& token : tokens)
            {
                auto parts = StringArray::fromTokens(token, ",", "");
                if (parts.size() == 2)
                {
                    int pluginChannel = parts[0].getIntValue();
                    int obsChannel = parts[1].getIntValue();
                    if (pluginChannel >= 0
                        && pluginChannel < (int)outputMapping.size()
                        && obsChannel >= 0
                        && obsChannel < (int)outputMapping[pluginChannel].size())
                    {
                        outputMapping[pluginChannel][obsChannel] = true;
                    }
                }
            }

            routingMatrix.setOutputMapping(outputMapping);
        }
    }

    if (auto* pluginNode = xml->getChildByName("PLUGIN"))
    {
        PluginDescription pd;
        pd.loadFromXml(*pluginNode);

        // Fix VST3 path on Linux
        if (pd.pluginFormatName == "VST3" && pd.fileOrIdentifier.contains("/Contents/"))
            pd.fileOrIdentifier = pd.fileOrIdentifier.upToLastOccurrenceOf(".vst3", true, false);

        MemoryBlock innerState;
        innerState.fromBase64Encoding(xml->getChildElementAllSubText(innerStateTag, {}));

        setNewPlugin(pd, (EditorStyle)xml->getIntAttribute(editorStyleTag, 0), innerState);
    }
}

void HostAudioProcessorImpl::setNewPlugin(const PluginDescription& pd, EditorStyle where, const MemoryBlock& mb)
{
    const ScopedLock sl(innerMutex);

    const auto callback = [this, where, mb](std::unique_ptr<AudioPluginInstance> instance, const String& error)
    {
        if (error.isNotEmpty())
        {
            auto options =
                MessageBoxOptions::makeOptionsOk(MessageBoxIconType::WarningIcon, "Plugin Load Failed", error);
            messageBox = AlertWindow::showScopedAsync(options, nullptr);
            return;
        }

        bool needsPluginChanged = false;
        String innerName;
        if (inner != nullptr)
            innerName = inner->getPluginDescription().descriptiveName;

        if (innerName != instance->getPluginDescription().descriptiveName)
            needsPluginChanged = true;

        if (needsPluginChanged)
            inner = std::move(instance);

        editorStyle = where;

        if (active)
        {
            bool layoutSupported = false;
            auto layout = getBusesLayout();

            // Try various bus layouts
            if (inner->checkBusesLayoutSupported(layout))
            {
                inner->setBusesLayout(layout);
                inner->setRateAndBufferSizeDetails(getSampleRate(), getBlockSize());
                layoutSupported = true;
            }

            if (!layoutSupported && layout.inputBuses.size() > 1)
            {
                layout.inputBuses.removeLast();
                layout.inputBuses.add(AudioChannelSet::stereo());
                if (inner->checkBusesLayoutSupported(layout))
                {
                    inner->setBusesLayout(layout);
                    inner->setRateAndBufferSizeDetails(getSampleRate(), getBlockSize());
                    layoutSupported = true;
                }
            }

            if (!layoutSupported && layout.inputBuses.size() > 1)
            {
                layout.inputBuses.removeLast();
                layout.inputBuses.add(AudioChannelSet::mono());
                if (inner->checkBusesLayoutSupported(layout))
                {
                    inner->setBusesLayout(layout);
                    inner->setRateAndBufferSizeDetails(getSampleRate(), getBlockSize());
                    layoutSupported = true;
                }
            }

            if (!layoutSupported && layout.inputBuses.size() > 1)
            {
                layout.inputBuses.removeLast();
                if (inner->checkBusesLayoutSupported(layout))
                {
                    inner->setBusesLayout(layout);
                    inner->setRateAndBufferSizeDetails(getSampleRate(), getBlockSize());
                    layoutSupported = true;
                }
            }

            if (!layoutSupported)
            {
                auto pluginInputs = inner->getTotalNumInputChannels();
                auto pluginOutputs = inner->getTotalNumOutputChannels();
                auto hostInputs = getMainBusNumInputChannels();
                auto hostOutputs = getMainBusNumOutputChannels();

                inner->setPlayConfigDetails(
                    jmin(pluginInputs, hostInputs),
                    jmin(pluginOutputs, hostOutputs),
                    getSampleRate(),
                    getBlockSize()
                );
            }
        }

        this->prepareToPlay(getSampleRate(), getBlockSize());

        if (inner != nullptr && !mb.isEmpty())
            inner->setStateInformation(mb.getData(), (int)mb.getSize());

        if (needsPluginChanged)
            NullCheckedInvocation::invoke(pluginChanged);
    };

    if (inner == nullptr || (inner != nullptr && inner->getPluginDescription().name != pd.name))
        pluginFormatManager.createPluginInstanceAsync(pd, getSampleRate(), getBlockSize(), callback);
}

void HostAudioProcessorImpl::clearPlugin()
{
    const ScopedLock sl(innerMutex);
    inner = nullptr;
    NullCheckedInvocation::invoke(pluginChanged);
}

bool HostAudioProcessorImpl::isPluginLoaded() const
{
    const ScopedLock sl(innerMutex);
    return inner != nullptr;
}

std::unique_ptr<AudioProcessorEditor> HostAudioProcessorImpl::createInnerEditor() const
{
    const ScopedLock sl(innerMutex);
    return rawToUniquePtr(inner->hasEditor() ? inner->createEditorIfNeeded() : nullptr);
}

EditorStyle HostAudioProcessorImpl::getEditorStyle() const noexcept
{
    return editorStyle;
}

juce::AudioPluginInstance* HostAudioProcessorImpl::getInnerPlugin() const
{
    const ScopedLock sl(innerMutex);
    return inner.get();
}

void HostAudioProcessorImpl::changeListenerCallback(ChangeBroadcaster* source)
{
    if (source != &pluginList)
        return;

    if (auto savedPluginList = pluginList.createXml())
    {
        appProperties.getUserSettings()->setValue("pluginList", savedPluginList.get());
        appProperties.saveIfNeeded();
    }
}

//==============================================================================
// HostAudioProcessor implementation
//==============================================================================
bool HostAudioProcessor::hasEditor() const
{
    return true;
}

AudioProcessorEditor* HostAudioProcessor::createEditor()
{
    return new HostAudioProcessorEditor(*this);
}
