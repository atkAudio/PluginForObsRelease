#pragma once

#include "AudioServer.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace atk
{

class AudioServerSettingsComponent
    : public juce::Component
    , private juce::Timer
    , private juce::Button::Listener
{
public:
    explicit AudioServerSettingsComponent(AudioClient* client, int clientChannels = -1);
    ~AudioServerSettingsComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    AudioClientState getSubscriptionState() const;
    void setSubscriptionState(const AudioClientState& state, bool expandToSubscriptions = true);
    void applySubscriptions();

    void setClientChannelInfo(const juce::StringArray& channelNames, const juce::String& firstColumnName = "Routing");
    void setClientChannelInfo(
        const juce::StringArray& inputChannelNames,
        const juce::StringArray& outputChannelNames,
        const juce::String& firstColumnName = "Routing"
    );
    void setClientChannelCount(int numChannels, const juce::String& firstColumnName = "Routing");

    void setInputFixedTopRows(const juce::StringArray& names, bool defaultEnabled = true);
    void setOutputFixedTopRows(const juce::StringArray& names, bool defaultEnabled = true);

    std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>> getObsChannelMappings() const;
    std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>> getCompleteRoutingMatrices() const;

    void setObsChannelMappings(
        const std::vector<std::vector<bool>>& inputMapping,
        const std::vector<std::vector<bool>>& outputMapping
    );
    void setCompleteRoutingMatrices(
        const std::vector<std::vector<bool>>& inputMapping,
        const std::vector<std::vector<bool>>& outputMapping
    );

    std::function<void(const std::vector<std::vector<bool>>&, const std::vector<std::vector<bool>>&)>
        onObsMappingChanged;
    std::function<std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>>()> getCurrentObsMappings;

private:
    void buttonClicked(juce::Button* button) override;

    // Tree model for device/channel hierarchy
    class DeviceChannelTreeItem : public juce::TreeViewItem
    {
    public:
        enum class ItemType
        {
            DeviceType,
            Device,
            Channel
        };

        DeviceChannelTreeItem(
            const juce::String& name,
            ItemType type,
            const juce::String& deviceName = {},
            int channelIndex = -1,
            bool isInput = true
        );

        bool mightContainSubItems() override;
        void paintItem(juce::Graphics& g, int width, int height) override;
        void itemClicked(const juce::MouseEvent& e) override;
        void itemOpennessChanged(bool isNowOpen) override;
        std::unique_ptr<juce::Component> createItemComponent() override;

        ItemType getType() const
        {
            return itemType;
        }

        juce::String getItemName() const
        {
            return itemName;
        }

        juce::String getDeviceName() const
        {
            return deviceName;
        }

        int getChannelIndex() const
        {
            return channelIndex;
        }

        bool isInputChannel() const
        {
            return isInput;
        }

        bool isSubscribed() const
        {
            return subscribed;
        }

        void setSubscribed(bool shouldBeSubscribed);

        void setServerInstance(AudioServer* serverInstance)
        {
            server = serverInstance;
        }

        void setSettingsComponent(AudioServerSettingsComponent* settingsComp)
        {
            settingsComponent = settingsComp;
        }

        bool isInputDevice() const
        {
            return isInput;
        }

        void resetChildrenLoadedFlag()
        {
            childrenLoaded = false;
        }

    private:
        juce::String itemName;
        ItemType itemType;
        juce::String deviceName;
        int channelIndex;
        bool isInput;
        bool subscribed = false;
        AudioServer* server = nullptr;
        AudioServerSettingsComponent* settingsComponent = nullptr;
        bool childrenLoaded = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceChannelTreeItem)
    };

    // Channel mapping matrix component using TableListBox
    class ChannelMappingMatrix
        : public juce::Component
        , public juce::TableListBoxModel
    {
    public:
        ChannelMappingMatrix();

        // TableListBoxModel overrides
        int getNumRows() override;
        void paintRowBackground(juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected) override;
        void
        paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;
        void cellClicked(int rowNumber, int columnId, const juce::MouseEvent& e) override;
        juce::String getCellTooltip(int rowNumber, int columnId) override;

        void resized() override;

        struct MappingRow
        {
            juce::String deviceName;
            juce::String deviceType;
            int deviceChannel;
            bool isInput;
        };

        void setSubscribedChannels(const std::vector<MappingRow>& rows);
        void setNumClientChannels(int numChannels);
        void setClientChannelNames(const juce::StringArray& names);
        void setFirstColumnName(const juce::String& name);

        void setFixedTopRows(const juce::StringArray& names, bool defaultEnabled = true);
        std::vector<std::vector<bool>> getFixedRowMappings() const;
        std::vector<std::vector<bool>> getCompleteRoutingMatrix() const;
        void setFixedRowMappings(const std::vector<std::vector<bool>>& mappings);
        void setCompleteMatrix(const std::vector<std::vector<bool>>& mappings);
        void resetFixedRowsToDefault();
        void clearSubscribedRowMappings();

        std::vector<ChannelMapping> getInputMappings() const;
        std::vector<ChannelMapping> getOutputMappings() const;
        void setMappings(
            const std::vector<ChannelMapping>& inputMappings,
            const std::vector<ChannelMapping>& outputMappings
        );

        const std::vector<MappingRow>& getSubscribedChannels() const
        {
            return subscribedChannels;
        }

    private:
        juce::TableListBox table;
        std::vector<MappingRow> subscribedChannels;
        std::vector<std::vector<bool>> mappingGrid;
        int numClientChannels = 0;
        int numFixedTopRows = 0;
        juce::StringArray clientChannelNames;
        juce::StringArray fixedRowNames;
        juce::String firstColumnName = "Routing";

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelMappingMatrix)
    };

    void updateDeviceTrees();
    void updateMappingMatrix();
    void onTreeSelectionChanged();
    void timerCallback() override;
    void clearAllDeviceSubscriptions(DeviceChannelTreeItem* root);

    AudioClient* client;
    AudioServer* server;

    // UI Layout
    juce::Label inputTreeLabel;
    std::unique_ptr<juce::TreeView> inputTreeView;
    std::unique_ptr<DeviceChannelTreeItem> inputRootItem;

    juce::Label outputTreeLabel;
    std::unique_ptr<juce::TreeView> outputTreeView;
    std::unique_ptr<DeviceChannelTreeItem> outputRootItem;

    juce::Label inputMatrixLabel;
    std::unique_ptr<ChannelMappingMatrix> inputMappingMatrix;

    juce::Label outputMatrixLabel;
    std::unique_ptr<ChannelMappingMatrix> outputMappingMatrix;

    juce::TextButton applyButton{"Apply"};
    juce::TextButton restoreButton{"Discard"};
    juce::TextButton cancelButton{"Reset"};
    juce::TextButton deviceButton{"Device..."};

    juce::AudioDeviceManager* externalDeviceManager = nullptr;
    juce::Component::SafePointer<juce::DialogWindow> deviceSettingsDialog;

    juce::String currentDeviceName; // Track which device we're showing settings for

    void updateDeviceSettings(const juce::String& deviceName);
    void showDeviceSettings();

public:
    void setDeviceManager(juce::AudioDeviceManager* manager)
    {
        externalDeviceManager = manager;
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioServerSettingsComponent)
};

} // namespace atk
