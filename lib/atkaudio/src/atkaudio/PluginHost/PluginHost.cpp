#include "../PluginHost.h"

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

struct atk::PluginHost::Impl : public juce::Timer
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
        startTimerHz(30);
    }

    ~Impl()
    {
        stopTimer();
        auto* window = this->mainWindow.release();
        auto lambda = [window] { delete window; };
        juce::MessageManager::callAsync(lambda);
    }

    void timerCallback() override
    {
        if (!isPrepared.load(std::memory_order_acquire))
        {
            auto* processor = mainWindow->getAudioProcessor();
            if (needsRelease)
                processor->releaseResources();

            processor->setPlayConfigDetails(numChannels, numChannels, sampleRate, numSamples);
            processor->prepareToPlay(sampleRate, numSamples);
            needsRelease = true;

            auto layout = juce::AudioProcessor::BusesLayout();

            layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(numChannels));
            layout.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(numChannels));
            layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(numChannels));

            if (processor->checkBusesLayoutSupported(layout))
            {
                processor->setBusesLayout(layout);
                processor->setRateAndBufferSizeDetails(sampleRate, numSamples);
            }
            isPrepared.store(true, std::memory_order_release);
        }
    }

    void process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
    {
        if (!buffer || this->numChannels != newNumChannels || this->numSamples < newNumSamples ||
            this->sampleRate != newSampleRate || isFirstRun)
        {
            isFirstRun = false;
            this->numChannels = newNumChannels;
            this->numSamples = newNumSamples;
            this->sampleRate = newSampleRate;
            isPrepared.store(false, std::memory_order_release);
            return;
        }

        if (!isPrepared.load(std::memory_order_acquire))
            return;

        if (!mainWindow->getPluginHolderLock().tryEnter())
            return;

        auto* processor = mainWindow->getAudioProcessor();

        audioBuffer.setDataToReferTo(buffer, newNumChannels * 2, newNumSamples);
        processor->getCallbackLock().enter();
        processor->processBlock(audioBuffer, midiBuffer);
        processor->getCallbackLock().exit();

        mainWindow->getPluginHolderLock().exit();
    }

    void setVisible(bool visible)
    {
        if (!mainWindow->isOnDesktop())
            mainWindow->addToDesktop(juce::ComponentPeer::StyleFlags{});

        mainWindow->setVisible(visible);
        if (visible && mainWindow->isMinimised())
            mainWindow->setMinimised(false);
    }

    void getState(std::string& s)
    {
        auto* processor = mainWindow->getAudioProcessor();
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        auto capacity = s.capacity();
        auto stateString = state.toString().toStdString();
        auto stateStringSize = stateString.size();

        if (stateStringSize > capacity)
            return;

        s = stateString;
    }

    void setState(std::string& s)
    {
        if (s.empty())
            return;

        juce::ScopedLock lock(mainWindow->getPluginHolderLock());
        auto* processor = mainWindow->getAudioProcessor();
        processor->setStateInformation(s.data(), (int)s.size());
    }

private:
    std::unique_ptr<StandaloneFilterWindow> mainWindow;

    juce::AudioBuffer<float> audioBuffer;
    juce::MidiBuffer midiBuffer;

    int numChannels = 2;
    int numSamples = 256;
    double sampleRate = 48000.0;

    std::atomic_bool isPrepared{false};
    bool isFirstRun = true;
    bool needsRelease = false;
};

void atk::PluginHost::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::PluginHost::setVisible(bool visible)
{
    pImpl->setVisible(visible);
}

void atk::PluginHost::getState(std::string& s)
{
    pImpl->getState(s);
}

void atk::PluginHost::setState(std::string& s)
{
    pImpl->setState(s);
}

atk::PluginHost::PluginHost()
    : pImpl(new Impl())
{
}

atk::PluginHost::~PluginHost()
{
    delete pImpl;
}
