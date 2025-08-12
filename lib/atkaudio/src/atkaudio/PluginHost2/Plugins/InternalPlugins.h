#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
using namespace juce;

#include "../ObsOutput.h"
#include "../ObsSource.h"
#include "../Ph2DeviceIo/Ph2DeviceIo.h"
#include "MidiGainPlugin.h"
#include "PluginGraph.h"

//==============================================================================
/**
    Manages the internal plugin types.
*/
class InternalPluginFormat final : public AudioPluginFormat
{
public:
    //==============================================================================
    InternalPluginFormat();

    //==============================================================================
    const std::vector<PluginDescription>& getAllTypes() const;

    //==============================================================================
    static String getIdentifier()
    {
        return "Internal";
    }

    String getName() const override
    {
        return getIdentifier();
    }

    bool fileMightContainThisPluginType(const String&) override
    {
        return true;
    }

    FileSearchPath getDefaultLocationsToSearch() override
    {
        return {};
    }

    bool canScanForPlugins() const override
    {
        return false;
    }

    bool isTrivialToScan() const override
    {
        return true;
    }

    void findAllTypesForFile(OwnedArray<PluginDescription>&, const String&) override
    {
    }

    bool doesPluginStillExist(const PluginDescription&) override
    {
        return true;
    }

    String getNameOfPluginFromIdentifier(const String& fileOrIdentifier) override
    {
        return fileOrIdentifier;
    }

    bool pluginNeedsRescanning(const PluginDescription&) override
    {
        return false;
    }

    StringArray searchPathsForPlugins(const FileSearchPath&, bool, bool) override
    {
        return {};
    }

private:
    class InternalPluginFactory
    {
    public:
        using Constructor = std::function<std::unique_ptr<AudioPluginInstance>()>;

        explicit InternalPluginFactory(const std::initializer_list<Constructor>& constructorsIn);

        const std::vector<PluginDescription>& getDescriptions() const
        {
            return descriptions;
        }

        std::unique_ptr<AudioPluginInstance> createInstance(const String& name) const;

    private:
        const std::vector<Constructor> constructors;
        const std::vector<PluginDescription> descriptions;
    };

    //==============================================================================
    void createPluginInstance(
        const PluginDescription&,
        double initialSampleRate,
        int initialBufferSize,
        PluginCreationCallback
    ) override;

    std::unique_ptr<AudioPluginInstance> createInstance(const String& name);

    bool requiresUnblockedMessageThreadDuringCreation(const PluginDescription&) const override;

    InternalPluginFactory factory;
};
