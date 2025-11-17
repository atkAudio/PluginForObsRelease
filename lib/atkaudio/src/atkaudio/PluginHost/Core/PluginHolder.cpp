#include "PluginHolder.h"
#include <atkaudio/atkaudio.h>

#ifndef DOXYGEN
#include <juce_audio_plugin_client/detail/juce_CreatePluginFilter.h>
#endif

using namespace juce;

extern AudioProcessor* JUCE_CALLTYPE createPluginFilter();

PluginHolder::PluginHolder(
    PropertySet* settingsToUse,
    bool takeOwnershipOfSettings,
    const String& preferredDefaultDeviceName,
    const AudioDeviceManager::AudioDeviceSetup* preferredSetupOptions,
    const Array<PluginInOuts>& channels,
    bool shouldAutoOpenMidiDevices
)
    : settings(settingsToUse, takeOwnershipOfSettings)
    , channelConfiguration(channels)
    , autoOpenMidiDevices(shouldAutoOpenMidiDevices)
{
    handleCreatePlugin();

    auto inChannels =
        (channelConfiguration.size() > 0 ? channelConfiguration[0].numIns : processor->getMainBusNumInputChannels());

    if (preferredSetupOptions != nullptr)
        options.reset(new AudioDeviceManager::AudioDeviceSetup(*preferredSetupOptions));

    auto audioInputRequired = (inChannels > 0);

    if (audioInputRequired
        && RuntimePermissions::isRequired(RuntimePermissions::recordAudio)
        && !RuntimePermissions::isGranted(RuntimePermissions::recordAudio))
    {
        RuntimePermissions::request(
            RuntimePermissions::recordAudio,
            [this, preferredDefaultDeviceName](bool granted) { init(granted, preferredDefaultDeviceName); }
        );
    }
    else
    {
        init(audioInputRequired, preferredDefaultDeviceName);
    }
}

PluginHolder::~PluginHolder()
{
    stopTimer();

    auto* hostAudioProcessor = dynamic_cast<HostAudioProcessorImpl*>(processor.get());
    if (hostAudioProcessor != nullptr)
        hostAudioProcessor->clearPlugin();

    handleDeletePlugin();
}

void PluginHolder::init(bool enableAudioInput, const String& preferredDefaultDeviceName)
{
    (void)enableAudioInput;
    (void)preferredDefaultDeviceName;

    reloadPluginState();
    startPlaying();

    if (autoOpenMidiDevices)
        startTimer(500);
}

void PluginHolder::createPlugin()
{
    handleCreatePlugin();
}

void PluginHolder::deletePlugin()
{
    handleDeletePlugin();
}

int PluginHolder::getNumInputChannels() const
{
    if (processor == nullptr)
        return 0;

    return (channelConfiguration.size() > 0 ? channelConfiguration[0].numIns : processor->getMainBusNumInputChannels());
}

int PluginHolder::getNumOutputChannels() const
{
    if (processor == nullptr)
        return 0;

    return (
        channelConfiguration.size() > 0 ? channelConfiguration[0].numOuts : processor->getMainBusNumOutputChannels()
    );
}

HostAudioProcessorImpl* PluginHolder::getHostProcessor() const
{
    return dynamic_cast<HostAudioProcessorImpl*>(processor.get());
}

String PluginHolder::getFilePatterns(const String& fileSuffix)
{
    if (fileSuffix.isEmpty())
        return {};

    return (fileSuffix.startsWithChar('.') ? "*" : "*.") + fileSuffix;
}

Value& PluginHolder::getMuteInputValue()
{
    return shouldMuteInput;
}

bool PluginHolder::getProcessorHasPotentialFeedbackLoop() const
{
    return processorHasPotentialFeedbackLoop;
}

void PluginHolder::valueChanged(Value& value)
{
    muteInput = (bool)value.getValue();
}

File PluginHolder::getLastFile() const
{
    File f;

    if (settings != nullptr)
        f = File(settings->getValue("lastStateFile"));

    if (f == File())
        f = File::getSpecialLocation(File::userDocumentsDirectory);

    return f;
}

void PluginHolder::setLastFile(const FileChooser& fc)
{
    if (settings != nullptr)
        settings->setValue("lastStateFile", fc.getResult().getFullPathName());
}

void PluginHolder::askUserToSaveState(const String& fileSuffix)
{
    stateFileChooser =
        std::make_unique<FileChooser>(TRANS("Save current state"), getLastFile(), getFilePatterns(fileSuffix));
    auto flags = FileBrowserComponent::saveMode
               | FileBrowserComponent::canSelectFiles
               | FileBrowserComponent::warnAboutOverwriting;

    stateFileChooser->launchAsync(
        flags,
        [this](const FileChooser& fc)
        {
            if (fc.getResult() == File{})
                return;

            setLastFile(fc);

            MemoryBlock data;
            processor->getStateInformation(data);

            if (!fc.getResult().replaceWithData(data.getData(), data.getSize()))
            {
                auto opts = MessageBoxOptions::makeOptionsOk(
                    AlertWindow::WarningIcon,
                    TRANS("Error whilst saving"),
                    TRANS("Couldn't write to the specified file!")
                );
                messageBox = AlertWindow::showScopedAsync(opts, nullptr);
            }
        }
    );
}

void PluginHolder::askUserToLoadState(const String& fileSuffix)
{
    stateFileChooser =
        std::make_unique<FileChooser>(TRANS("Load a saved state"), getLastFile(), getFilePatterns(fileSuffix));
    auto flags = FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles;

    stateFileChooser->launchAsync(
        flags,
        [this](const FileChooser& fc)
        {
            if (fc.getResult() == File{})
                return;

            setLastFile(fc);

            MemoryBlock data;

            if (fc.getResult().loadFileAsData(data))
            {
                processor->setStateInformation(data.getData(), (int)data.getSize());
            }
            else
            {
                auto opts = MessageBoxOptions::makeOptionsOk(
                    AlertWindow::WarningIcon,
                    TRANS("Error whilst loading"),
                    TRANS("Couldn't read from the specified file!")
                );
                messageBox = AlertWindow::showScopedAsync(opts, nullptr);
            }
        }
    );
}

void PluginHolder::startPlaying()
{
#if JucePlugin_Enable_IAA && JUCE_IOS
    if (auto device = dynamic_cast<iOSAudioIODevice*>(deviceManager.getCurrentAudioDevice()))
    {
        processor->setPlayHead(device->getAudioPlayHead());
        device->setMidiMessageCollector(&player.getMidiMessageCollector());
    }
#endif
}

void PluginHolder::stopPlaying()
{
}

void PluginHolder::savePluginState()
{
    if (settings != nullptr && processor != nullptr)
    {
        MemoryBlock data;
        processor->getStateInformation(data);

        settings->setValue("filterState", data.toBase64Encoding());
    }
}

void PluginHolder::reloadPluginState()
{
    if (settings != nullptr)
    {
        MemoryBlock data;

        if (data.fromBase64Encoding(settings->getValue("filterState")) && data.getSize() > 0)
            processor->setStateInformation(data.getData(), (int)data.getSize());
    }
}

void PluginHolder::handleCreatePlugin()
{
    processor.reset(createPluginFilter());
    processor->setRateAndBufferSizeDetails(48000, MAX_OBS_AUDIO_BUFFER_SIZE);

    processorHasPotentialFeedbackLoop = (getNumInputChannels() > 0 && getNumOutputChannels() > 0);
}

void PluginHolder::handleDeletePlugin()
{
    stopPlaying();
    processor = nullptr;
}

void PluginHolder::timerCallback()
{
}
