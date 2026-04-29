#include "HostAudioProcessor.h"
#include "../../SharedPluginList.h"
#include "../UI/HostEditorWindow.h"

using namespace juce;
using juce::NullCheckedInvocation;

juce::Optional<juce::AudioPlayHead::PositionInfo> HostAudioProcessorImpl::AtkAudioPlayHead::getPosition() const
{
    return positionInfo;
}

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

    juce::addDefaultFormatsToManager(pluginFormatManager);

#if JUCE_LINUX
    {
        const File flatpakPluginPath("/app/extensions/Plugins");
        if (flatpakPluginPath.isDirectory())
        {
            if (auto* props = atk::SharedPluginList::getInstance()->getPropertiesFile())
            {
                for (auto* format : pluginFormatManager.getFormats())
                {
                    const String formatName = format->getName();
                    // JUCE uses "lastPluginScanPath_" prefix for PluginListComponent
                    const String key = "lastPluginScanPath_" + formatName;
                    FileSearchPath existingPaths(
                        props->getValue(key, format->getDefaultLocationsToSearch().toString())
                    );

                    if (!existingPaths.toString().contains(flatpakPluginPath.getFullPathName()))
                    {
                        existingPaths.add(flatpakPluginPath);
                        props->setValue(key, existingPaths.toString());
                    }
                }
            }
        }
    }
#endif

    atk::SharedPluginList::getInstance()->loadPluginList(pluginList, true);
    pluginList.addChangeListener(this);

    routingMatrix.initializeDefaultMapping(numChannels * 2);

    DBG("[MIDI_SRV] PluginHost created with MidiClient");
}

HostAudioProcessorImpl::~HostAudioProcessorImpl()
{
    pluginList.removeChangeListener(this);
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

    const int maxChannels = 32;
    const int maxSamples = bs * 2;
    const int maxSubscriptions = 16;

    if (internalBuffer.getNumChannels() < maxChannels || internalBuffer.getNumSamples() < maxSamples)
        internalBuffer.setSize(maxChannels, maxSamples, false, false, true);

    if (deviceInputBuffer.getNumChannels() < maxSubscriptions || deviceInputBuffer.getNumSamples() < maxSamples)
        deviceInputBuffer.setSize(maxSubscriptions, maxSamples, false, false, true);

    if (deviceOutputBuffer.getNumChannels() < maxSubscriptions || deviceOutputBuffer.getNumSamples() < maxSamples)
        deviceOutputBuffer.setSize(maxSubscriptions, maxSamples, false, false, true);

    inputMidiCopy.ensureSize(2048);
}

void HostAudioProcessorImpl::releaseResources()
{
    const ScopedLock sl(innerMutex);

    active = false;

    if (inner != nullptr)
    {
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
    const ScopedTryLock sl(innerMutex);
    if (!sl.isLocked())
        return;

    jassert(!isUsingDoublePrecision());
    if (inner == nullptr)
        return;

    AudioBuffer<float> tempBuffer;

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

    int numInputSubs = audioClient.getNumInputSubscriptions();
    int numOutputSubs = audioClient.getNumOutputSubscriptions();

    int pluginChannels = buffer.getNumChannels();
    if (internalBuffer.getNumChannels() < pluginChannels || internalBuffer.getNumSamples() < buffer.getNumSamples())
        internalBuffer.setSize(pluginChannels, buffer.getNumSamples(), false, false, true);

    if (deviceInputBuffer.getNumChannels() < numInputSubs || deviceInputBuffer.getNumSamples() < buffer.getNumSamples())
        deviceInputBuffer.setSize(std::max(numInputSubs, 1), buffer.getNumSamples(), false, false, true);

    if (deviceOutputBuffer.getNumChannels() < numOutputSubs
        || deviceOutputBuffer.getNumSamples() < buffer.getNumSamples())
        deviceOutputBuffer.setSize(std::max(numOutputSubs, 1), buffer.getNumSamples(), false, false, true);

    audioClient.pullSubscribedInputs(deviceInputBuffer, buffer.getNumSamples(), getSampleRate());

    routingMatrix.applyInputRouting(
        buffer.getArrayOfWritePointers(),
        deviceInputBuffer,
        internalBuffer,
        buffer.getNumChannels(),
        buffer.getNumSamples(),
        numInputSubs
    );

    midiClient.getPendingMidi(midiBuffer, buffer.getNumSamples(), getSampleRate());

    inputMidiCopy.clear();
    inputMidiCopy.addEvents(midiBuffer, 0, buffer.getNumSamples(), 0);

    tempBuffer.setDataToReferTo(
        internalBuffer.getArrayOfWritePointers(),
        internalBuffer.getNumChannels(),
        buffer.getNumSamples()
    );

    if (inner->isSuspended())
        tempBuffer.clear();
    else
        inner->processBlock(tempBuffer, midiBuffer);

    routingMatrix.applyOutputRouting(
        internalBuffer,
        buffer.getArrayOfWritePointers(),
        deviceOutputBuffer,
        buffer.getNumChannels(),
        buffer.getNumSamples(),
        numOutputSubs
    );

    audioClient.pushSubscribedOutputs(deviceOutputBuffer, buffer.getNumSamples(), getSampleRate());

    MidiBuffer* outputMidiToSend = nullptr;
    if (inner->isMidiEffect())
        outputMidiToSend = &midiBuffer;
    else
        outputMidiToSend = &inputMidiCopy;

    if (!outputMidiToSend->isEmpty())
        midiClient.sendMidi(*outputMidiToSend);
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

void HostAudioProcessorImpl::getStateInformation(MemoryBlock& destData)
{
    XmlElement xml("state");

    auto audioState = audioClient.getSubscriptions();
    auto audioStateStr = audioState.serialize();
    DBG("HostAudioProcessor: Saving audio state: " << audioStateStr);
    xml.setAttribute("audioClientState", audioStateStr);

    auto midiState = midiClient.getSubscriptions();
    xml.setAttribute("midiClientState", midiState.serialize());

    {
        auto inputMapping = routingMatrix.getInputMapping();
        auto outputMapping = routingMatrix.getOutputMapping();

        if (!inputMapping.empty())
        {
            auto inputMappingElement = std::make_unique<XmlElement>("InputMapping");
            for (const auto& row : inputMapping)
            {
                String rowData;
                for (bool cell : row)
                    rowData += cell ? '1' : '0';

                auto rowElement = std::make_unique<XmlElement>("Row");
                rowElement->setAttribute("data", rowData);
                inputMappingElement->addChildElement(rowElement.release());
            }
            xml.addChildElement(inputMappingElement.release());
        }

        if (!outputMapping.empty())
        {
            auto outputMappingElement = std::make_unique<XmlElement>("OutputMapping");
            for (const auto& row : outputMapping)
            {
                String rowData;
                for (bool cell : row)
                    rowData += cell ? '1' : '0';

                auto rowElement = std::make_unique<XmlElement>("Row");
                rowElement->setAttribute("data", rowData);
                outputMappingElement->addChildElement(rowElement.release());
            }
            xml.addChildElement(outputMappingElement.release());
        }
    }

    if (inner != nullptr)
    {
        xml.setAttribute(editorStyleTag, (int)editorStyle);

        auto pd = inner->getPluginDescription();

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

    if (xml->hasAttribute("audioClientState"))
    {
        auto audioStateStr = xml->getStringAttribute("audioClientState");
        DBG("HostAudioProcessor: Restoring audio state: " << audioStateStr);
        atk::AudioClientState audioState;
        audioState.deserialize(audioStateStr);
        audioClient.setSubscriptions(audioState);
    }

    if (xml->hasAttribute("midiClientState"))
    {
        atk::MidiClientState midiState;
        midiState.deserialize(xml->getStringAttribute("midiClientState"));
        midiClient.setSubscriptions(midiState);
    }

    {
        if (auto* inputMappingElement = xml->getChildByName("InputMapping"))
        {
            std::vector<std::vector<bool>> inputMapping;
            for (auto* rowElement : inputMappingElement->getChildIterator())
            {
                String rowData = rowElement->getStringAttribute("data");
                std::vector<bool> row;
                for (int i = 0; i < rowData.length(); ++i)
                    row.push_back(rowData[i] == '1');
                inputMapping.push_back(row);
            }
            if (!inputMapping.empty())
                routingMatrix.setInputMapping(inputMapping);
        }
        else if (xml->hasAttribute("inputChannelMapping"))
        {
            auto inputMapping = routingMatrix.getInputMapping();
            auto inputMappingStr = xml->getStringAttribute("inputChannelMapping");
            auto tokens = StringArray::fromTokens(inputMappingStr, ";", "");

            for (auto& row : inputMapping)
                std::fill(row.begin(), row.end(), false);

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

        if (auto* outputMappingElement = xml->getChildByName("OutputMapping"))
        {
            std::vector<std::vector<bool>> outputMapping;
            for (auto* rowElement : outputMappingElement->getChildIterator())
            {
                String rowData = rowElement->getStringAttribute("data");
                std::vector<bool> row;
                for (int i = 0; i < rowData.length(); ++i)
                    row.push_back(rowData[i] == '1');
                outputMapping.push_back(row);
            }
            if (!outputMapping.empty())
                routingMatrix.setOutputMapping(outputMapping);
        }
        else if (xml->hasAttribute("outputChannelMapping"))
        {
            auto outputMapping = routingMatrix.getOutputMapping();
            auto outputMappingStr = xml->getStringAttribute("outputChannelMapping");
            auto tokens = StringArray::fromTokens(outputMappingStr, ";", "");

            for (auto& row : outputMapping)
                std::fill(row.begin(), row.end(), false);

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
        const ScopedLock sl(innerMutex);
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
    // TODO: Temporarily avoiding innerMutex lock during editor creation to prevent audio glitches.
    // The lock was causing the audio thread's tryLock to fail while the UI thread held the mutex.
    // This is safe as long as inner pointer doesn't change during normal operation.
    AudioPluginInstance* pluginToUse = nullptr;
    {
        const ScopedLock sl(innerMutex);
        pluginToUse = inner.get();
    }
    if (pluginToUse != nullptr && pluginToUse->hasEditor())
        return rawToUniquePtr(pluginToUse->createEditorIfNeeded());
    return nullptr;
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
    if (source == &pluginList)
        atk::SharedPluginList::getInstance()->savePluginList(pluginList);
}

bool HostAudioProcessor::hasEditor() const
{
    return true;
}

AudioProcessorEditor* HostAudioProcessor::createEditor()
{
    return new HostAudioProcessorEditor(*this);
}
