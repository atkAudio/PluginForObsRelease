// Include DelayLinePlugin FIRST because juce_dsp SSE headers must come before OBS SIMDE headers
#include "DelayLinePlugin.h"

#include "InternalPlugin.h"
#include "InternalPlugins.h"

#include "DeviceIo2Plugin.h"
#include "ObsOutput.h"
#include "ObsSource.h"
#include "GainPlugin.h"
#include "ParentSource.h"
#include "Transport.h"
#include "ChordAnalyzerPlugin.h"
#include "../PluginGraph.h"

#include <config.h>
#include <juce_audio_plugin_client/juce_audio_plugin_client.h>

class SineWaveSynth final : public AudioProcessor
{
public:
    SineWaveSynth()
        : AudioProcessor(BusesProperties().withOutput("Output", AudioChannelSet::stereo()))
    {
        const int numVoices = 8;

        // Add some voices...
        for (int i = numVoices; --i >= 0;)
            synth.addVoice(new SineWaveVoice());

        // ..and give the synth a sound to play
        synth.addSound(new SineWaveSound());
    }

    static String getIdentifier()
    {
        return "Sine Wave Synth";
    }

    void prepareToPlay(double newSampleRate, int) override
    {
        synth.setCurrentPlaybackSampleRate(newSampleRate);
    }

    void releaseResources() override
    {
    }

    void processBlock(AudioBuffer<float>& buffer, MidiBuffer& midiMessages) override
    {
        const int numSamples = buffer.getNumSamples();

        buffer.clear();
        synth.renderNextBlock(buffer, midiMessages, 0, numSamples);
        buffer.applyGain(0.8f);
    }

    using AudioProcessor::processBlock;

    const String getName() const override
    {
        return getIdentifier();
    }

    double getTailLengthSeconds() const override
    {
        return 0.0;
    }

    bool acceptsMidi() const override
    {
        return true;
    }

    bool producesMidi() const override
    {
        return true;
    }

    AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }

    bool hasEditor() const override
    {
        return false;
    }

    int getNumPrograms() override
    {
        return 1;
    }

    int getCurrentProgram() override
    {
        return 0;
    }

    void setCurrentProgram(int) override
    {
    }

    const String getProgramName(int) override
    {
        return {};
    }

    void changeProgramName(int, const String&) override
    {
    }

    void getStateInformation(juce::MemoryBlock&) override
    {
    }

    void setStateInformation(const void*, int) override
    {
    }

private:
    struct SineWaveSound final : public SynthesiserSound
    {
        SineWaveSound() = default;

        bool appliesToNote(int /*midiNoteNumber*/) override
        {
            return true;
        }

        bool appliesToChannel(int /*midiChannel*/) override
        {
            return true;
        }
    };

    struct SineWaveVoice final : public SynthesiserVoice
    {
        SineWaveVoice() = default;

        bool canPlaySound(SynthesiserSound* sound) override
        {
            return dynamic_cast<SineWaveSound*>(sound) != nullptr;
        }

        void startNote(
            int midiNoteNumber,
            float velocity,
            SynthesiserSound* /*sound*/,
            int /*currentPitchWheelPosition*/
        ) override
        {
            currentAngle = 0.0;
            level = velocity * 0.15;
            tailOff = 0.0;

            double cyclesPerSecond = MidiMessage::getMidiNoteInHertz(midiNoteNumber);
            double cyclesPerSample = cyclesPerSecond / getSampleRate();

            angleDelta = cyclesPerSample * 2.0 * MathConstants<double>::pi;
        }

        void stopNote(float /*velocity*/, bool allowTailOff) override
        {
            if (allowTailOff)
            {
                // start a tail-off by setting this flag. The render callback will pick up on
                // this and do a fade out, calling clearCurrentNote() when it's finished.

                if (approximatelyEqual(tailOff, 0.0)) // we only need to begin a tail-off if it's not already doing so -
                                                      // the stopNote method could be called more than once.
                    tailOff = 1.0;
            }
            else
            {
                // we're being told to stop playing immediately, so reset everything..

                clearCurrentNote();
                angleDelta = 0.0;
            }
        }

        void pitchWheelMoved(int /*newValue*/) override
        {
            // not implemented for the purposes of this demo!
        }

        void controllerMoved(int /*controllerNumber*/, int /*newValue*/) override
        {
            // not implemented for the purposes of this demo!
        }

        void renderNextBlock(AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override
        {
            if (!approximatelyEqual(angleDelta, 0.0))
            {
                if (tailOff > 0)
                {
                    while (--numSamples >= 0)
                    {
                        const float currentSample = (float)(sin(currentAngle) * level * tailOff);

                        for (int i = outputBuffer.getNumChannels(); --i >= 0;)
                            outputBuffer.addSample(i, startSample, currentSample);

                        currentAngle += angleDelta;
                        ++startSample;

                        tailOff *= 0.99;

                        if (tailOff <= 0.005)
                        {
                            // tells the synth that this voice has stopped
                            clearCurrentNote();

                            angleDelta = 0.0;
                            break;
                        }
                    }
                }
                else
                {
                    while (--numSamples >= 0)
                    {
                        const float currentSample = (float)(sin(currentAngle) * level);

                        for (int i = outputBuffer.getNumChannels(); --i >= 0;)
                            outputBuffer.addSample(i, startSample, currentSample);

                        currentAngle += angleDelta;
                        ++startSample;
                    }
                }
            }
        }

        using SynthesiserVoice::renderNextBlock;

    private:
        double currentAngle = 0, angleDelta = 0, level = 0, tailOff = 0;
    };

    Synthesiser synth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SineWaveSynth)
};

class ReverbPlugin final : public AudioProcessor
{
public:
    ReverbPlugin()
        : AudioProcessor(
              BusesProperties()
                  .withInput("Input", AudioChannelSet::stereo())
                  .withOutput("Output", AudioChannelSet::stereo())
          )
    {
    }

    static String getIdentifier()
    {
        return "Reverb";
    }

    void prepareToPlay(double newSampleRate, int) override
    {
        reverb.setSampleRate(newSampleRate);
    }

    void reset() override
    {
        reverb.reset();
    }

    void releaseResources() override
    {
    }

    void processBlock(AudioBuffer<float>& buffer, MidiBuffer&) override
    {
        auto numChannels = buffer.getNumChannels();

        if (numChannels == 1)
            reverb.processMono(buffer.getWritePointer(0), buffer.getNumSamples());
        else
            reverb.processStereo(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples());

        for (int ch = 2; ch < numChannels; ++ch)
            buffer.clear(ch, 0, buffer.getNumSamples());
    }

    using AudioProcessor::processBlock;

    const String getName() const override
    {
        return getIdentifier();
    }

    double getTailLengthSeconds() const override
    {
        return 0.0;
    }

    bool acceptsMidi() const override
    {
        return false;
    }

    bool producesMidi() const override
    {
        return false;
    }

    AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }

    bool hasEditor() const override
    {
        return false;
    }

    int getNumPrograms() override
    {
        return 1;
    }

    int getCurrentProgram() override
    {
        return 0;
    }

    void setCurrentProgram(int) override
    {
    }

    const String getProgramName(int) override
    {
        return {};
    }

    void changeProgramName(int, const String&) override
    {
    }

    void getStateInformation(juce::MemoryBlock&) override
    {
    }

    void setStateInformation(const void*, int) override
    {
    }

private:
    Reverb reverb;
};

InternalPluginFormat::InternalPluginFactory::InternalPluginFactory(
    const std::initializer_list<Constructor>& constructorsIn
)
    : constructors(constructorsIn)
    , descriptions(
          [&]
          {
              std::vector<PluginDescription> result;

              for (const auto& constructor : constructors)
                  result.push_back(constructor()->getPluginDescription());

              return result;
          }()
      )
{
}

std::unique_ptr<AudioPluginInstance>
InternalPluginFormat::InternalPluginFactory::createInstance(const String& name) const
{
    const auto begin = descriptions.begin();
    const auto it = std::find_if(
        begin,
        descriptions.end(),
        [&](const PluginDescription& desc) { return name.equalsIgnoreCase(desc.name); }
    );

    if (it == descriptions.end())
        return nullptr;

    const auto index = (size_t)std::distance(begin, it);
    return constructors[index]();
}

InternalPluginFormat::InternalPluginFormat()
    : factory{
          []
          {
              return std::make_unique<AudioProcessorGraphMT::AudioGraphIOProcessor>(
                  AudioProcessorGraphMT::AudioGraphIOProcessor::audioInputNode
              );
          },
          []
          {
              return std::make_unique<AudioProcessorGraphMT::AudioGraphIOProcessor>(
                  AudioProcessorGraphMT::AudioGraphIOProcessor::midiInputNode
              );
          },
          []
          {
              return std::make_unique<AudioProcessorGraphMT::AudioGraphIOProcessor>(
                  AudioProcessorGraphMT::AudioGraphIOProcessor::audioOutputNode
              );
          },
          []
          {
              return std::make_unique<AudioProcessorGraphMT::AudioGraphIOProcessor>(
                  AudioProcessorGraphMT::AudioGraphIOProcessor::midiOutputNode
              );
          },

          [] { return std::make_unique<InternalPlugin>(std::make_unique<ObsSourceAudioProcessor>()); },
          [] { return std::make_unique<InternalPlugin>(std::make_unique<ObsOutputAudioProcessor>()); },
          [] { return std::make_unique<InternalPlugin>(std::make_unique<DeviceIo2Plugin>()); },
          [] { return std::make_unique<InternalPlugin>(std::make_unique<SineWaveSynth>()); },
          [] { return std::make_unique<InternalPlugin>(std::make_unique<ReverbPlugin>()); },
          [] { return std::make_unique<InternalPlugin>(std::make_unique<GainPlugin>()); },
          [] { return std::make_unique<InternalPlugin>(std::make_unique<DelayLinePlugin>()); },
          [] { return std::make_unique<TransportProcessor>(); },
          [] { return std::make_unique<ChordAnalyzerPlugin>(); },
          [] { return std::make_unique<atk::ParentSourceProcessor>(atk::ParentSourceProcessor::input); },
          [] { return std::make_unique<atk::ParentSourceProcessor>(atk::ParentSourceProcessor::output); }
      }
{
}

std::unique_ptr<AudioPluginInstance> InternalPluginFormat::createInstance(const String& name)
{
    return factory.createInstance(name);
}

void InternalPluginFormat::createPluginInstance(
    const PluginDescription& desc,
    double /*initialSampleRate*/,
    int /*initialBufferSize*/,
    PluginCreationCallback callback
)
{
    if (auto p = createInstance(desc.name))
        callback(std::move(p), {});
    else
        callback(nullptr, NEEDS_TRANS("Invalid internal plugin name"));
}

bool InternalPluginFormat::requiresUnblockedMessageThreadDuringCreation(const PluginDescription&) const
{
    return false;
}

void setParentSourceUuidOnInternalPlugin(AudioPluginInstance* plugin, const std::string& parentUuid)
{
    if (!plugin || parentUuid.empty())
        return;

    // Check if this is an InternalPlugin wrapping ObsSourceAudioProcessor
    if (auto* internalPlugin = dynamic_cast<InternalPlugin*>(plugin))
    {
        if (auto* obsSource = dynamic_cast<ObsSourceAudioProcessor*>(internalPlugin->getInnerProcessor()))
            obsSource->setParentSourceUuid(parentUuid);

        if (auto* deviceIo2 = dynamic_cast<DeviceIo2Plugin*>(internalPlugin->getInnerProcessor()))
            deviceIo2->setParentSourceUuid(parentUuid);
    }
}

const std::vector<PluginDescription>& InternalPluginFormat::getAllTypes() const
{
    return factory.getDescriptions();
}
