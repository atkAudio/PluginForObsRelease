#include "PluginHost.h"

#include "JuceHostPlugin.h"
#include "StandaloneFilterWindow.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HostAudioProcessor();
}

static std::unique_ptr<StandalonePluginHolder2> createPluginHolder()
{
    return std::make_unique<StandalonePluginHolder2>(nullptr);
}

struct atk::PluginHost::Impl
{
    Impl()
        : mainWindow(
              std::make_unique<StandaloneFilterWindow>(
                  "atkAudio Plugin Host",
                  LookAndFeel::getDefaultLookAndFeel().findColour(ResizableWindow::backgroundColourId),
                  createPluginHolder()
              )
          )
    {
    }

    ~Impl()
    {
        // Release resources before destruction
        if (isPrepared)
        {
            juce::ScopedLock lock(mainWindow->getPluginHolderLock());
            auto* processor = mainWindow->getAudioProcessor();
            juce::ScopedLock callbackLock(processor->getCallbackLock());
            processor->releaseResources();
        }

        auto* window = this->mainWindow.release();
        auto lambda = [window] { delete window; };
        juce::MessageManager::callAsync(lambda);
    }

    void prepareProcessor(int channels, int samples, double rate)
    {
        juce::ScopedLock lock(mainWindow->getPluginHolderLock());
        auto* processor = mainWindow->getAudioProcessor();
        juce::ScopedLock callbackLock(processor->getCallbackLock());

        // Release previous configuration if needed
        if (isPrepared)
            processor->releaseResources();

        // Configure bus layout: main bus + sidechain bus
        auto layout = juce::AudioProcessor::BusesLayout();
        layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        layout.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));

        // Apply layout and prepare
        if (processor->checkBusesLayoutSupported(layout))
        {
            processor->setBusesLayout(layout);
            processor->setRateAndBufferSizeDetails(rate, samples);
            processor->prepareToPlay(rate, samples);
            isPrepared = true;
        }
    }

    void process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
    {
        if (!buffer)
            return;

        // Check if we need to reconfigure the processor
        // Note: numSamples can vary between calls, only reallocate if we need MORE space
        bool needsReconfiguration =
            !isPrepared || numChannels != newNumChannels || numSamples < newNumSamples || sampleRate != newSampleRate;

        if (needsReconfiguration)
        {
            numChannels = newNumChannels;
            numSamples = juce::jmax(numSamples, newNumSamples); // Allocate for the largest size seen
            sampleRate = newSampleRate;

            // Schedule async preparation (can't block OBS audio thread)
            // Note: This is different from standalone JUCE apps where audioDeviceAboutToStart
            // is called before audio begins. In OBS plugins, we must handle dynamic changes.
            juce::MessageManager::callAsync(
                [this, channels = newNumChannels, samples = numSamples, rate = newSampleRate]
                { prepareProcessor(channels, samples, rate); }
            );

            // Skip this buffer while preparing
            return;
        }

        // Skip processing if not yet prepared
        if (!isPrepared)
            return;

        // Try to acquire lock without blocking (real-time safe)
        if (!mainWindow->getPluginHolderLock().tryEnter())
            return;

        auto* processor = mainWindow->getAudioProcessor();

        // Try to acquire callback lock (real-time safe)
        if (!processor->getCallbackLock().tryEnter())
        {
            mainWindow->getPluginHolderLock().exit();
            return;
        }

        // Process audio
        audioBuffer.setDataToReferTo(buffer, newNumChannels * 2, newNumSamples);

        // Clear MIDI buffer (we don't provide MIDI input, so ensure it's empty)
        midiBuffer.clear();

        if (!processor->isSuspended())
            processor->processBlock(audioBuffer, midiBuffer);

        processor->getCallbackLock().exit();
        mainWindow->getPluginHolderLock().exit();
    }

    juce::Component* getWindowComponent()
    {
        return mainWindow.get();
    }

    // some plugins dont export state if audio is not playing
    void getState(std::string& s)
    {
        juce::ScopedLock lock(mainWindow->getPluginHolderLock());
        auto* processor = mainWindow->getAudioProcessor();
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        auto stateString = state.toString().toStdString();

        s = stateString;
    }

    void setState(std::string& s)
    {
        if (s.empty())
            return;

        // Defer state restoration to ensure plugin list and formats are fully initialized
        juce::MessageManager::callAsync(
            [this, stateString = s]() mutable
            {
                juce::ScopedLock lock(mainWindow->getPluginHolderLock());
                auto* processor = mainWindow->getAudioProcessor();
                juce::ScopedLock callbackLock(processor->getCallbackLock());
                juce::MemoryBlock stateData(stateString.data(), stateString.size());
                processor->setStateInformation(stateData.getData(), (int)stateData.getSize());
            }
        );
    }

private:
    std::unique_ptr<StandaloneFilterWindow> mainWindow;

    juce::AudioBuffer<float> audioBuffer;
    juce::MidiBuffer midiBuffer;

    int numChannels = 0;
    int numSamples = 0;
    double sampleRate = 0.0;

    bool isPrepared = false;
};

void atk::PluginHost::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::PluginHost::getState(std::string& s)
{
    pImpl->getState(s);
}

void atk::PluginHost::setState(std::string& s)
{
    pImpl->setState(s);
}

juce::Component* atk::PluginHost::getWindowComponent()
{
    return pImpl->getWindowComponent();
}

atk::PluginHost::PluginHost()
    : pImpl(new Impl())
{
}

atk::PluginHost::~PluginHost()
{
    delete pImpl;
}
