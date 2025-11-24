#pragma once

#include "AudioServer.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace atk
{

/**
 * Settings component for managing Audio device subscriptions
 * Provides UI with tree views for device/channel selection and XY matrix for channel mapping
 */
class AudioServerSettingsComponent
    : public juce::Component
    , private juce::Timer
    , private juce::Button::Listener
{
public:
    /**
     * Create settings component for a specific Audio client
     * @param client The client whose subscriptions to manage
     * @param clientChannels Number of channels in the client (defaults to querying client if not specified)
     */
    explicit AudioServerSettingsComponent(AudioClient* client, int clientChannels = -1);
    ~AudioServerSettingsComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /**
     * Get the current subscription state from the UI
     */
    AudioClientState getSubscriptionState() const;

    /**
     * Set the subscription state in the UI
     * @param state The subscription state to restore
     * @param expandToSubscriptions If true, expand tree to show subscribed channels
     */
    void setSubscriptionState(const AudioClientState& state, bool expandToSubscriptions = true);

    /**
     * Apply current subscriptions to the client
     */
    void applySubscriptions();

    /**
     * Set the client channel information (count and names) for BOTH routing matrices
     * @param channelNames Array of channel names (size determines channel count)
     * @param firstColumnName Optional name for the first column (defaults to "Routing")
     */
    void setClientChannelInfo(const juce::StringArray& channelNames, const juce::String& firstColumnName = "Routing");

    /**
     * Set separate channel info for input and output routing matrices
     * @param inputChannelNames Channel names for input routing (device->plugin)
     * @param outputChannelNames Channel names for output routing (plugin->device)
     * @param firstColumnName Optional name for the first column (defaults to "Routing")
     */
    void setClientChannelInfo(
        const juce::StringArray& inputChannelNames,
        const juce::StringArray& outputChannelNames,
        const juce::String& firstColumnName = "Routing"
    );

    /**
     * Set the client channel count (uses default channel names like "Ch 1", "Ch 2")
     * @param numChannels Number of client channels for both matrices
     * @param firstColumnName Optional name for the first column (defaults to "Routing")
     */
    void setClientChannelCount(int numChannels, const juce::String& firstColumnName = "Routing");

    /**
     * Set fixed top rows for INPUT routing matrix (e.g., OBS channels)
     * @param names Channel names for the fixed rows
     * @param defaultEnabled If true, creates diagonal pattern by default
     */
    void setInputFixedTopRows(const juce::StringArray& names, bool defaultEnabled = true);

    /**
     * Set fixed top rows for OUTPUT routing matrix (e.g., OBS channels)
     * @param names Channel names for the fixed rows
     * @param defaultEnabled If true, creates diagonal pattern by default
     */
    void setOutputFixedTopRows(const juce::StringArray& names, bool defaultEnabled = true);

    /**
     * Get the OBS channel mapping from fixed top rows in input and output matrices
     * @return pair of 2D vectors: first=input [obsChannel][pluginChannel], second=output [pluginChannel][obsChannel]
     */
    std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>> getObsChannelMappings() const;

    /**
     * Get the complete routing matrices including both OBS channels and device subscriptions
     * @return pair of (inputMatrix, outputMatrix) where each matrix is [sourceChannel][pluginChannel]
     *         Input: sourceChannel = OBS channels + input device subscriptions
     *         Output: sourceChannel = plugin channels (rows), pluginChannel = OBS + output device subscriptions (cols)
     */
    std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>> getCompleteRoutingMatrices() const;

    /**
     * Set the OBS channel mapping for fixed top rows
     * @param inputMapping Input mapping [obsChannel][pluginChannel]
     * @param outputMapping Output mapping [pluginChannel][obsChannel]
     */
    void setObsChannelMappings(
        const std::vector<std::vector<bool>>& inputMapping,
        const std::vector<std::vector<bool>>& outputMapping
    );

    /**
     * Set the complete routing matrices (OBS channels + device subscriptions)
     * This sets ALL rows in the matrix, including both fixed top rows and subscription rows
     * @param inputMapping Complete input mapping matrix
     * @param outputMapping Complete output mapping matrix
     */
    void setCompleteRoutingMatrices(
        const std::vector<std::vector<bool>>& inputMapping,
        const std::vector<std::vector<bool>>& outputMapping
    );

    /**
     * Set callback to be called when OBS channel mapping changes (on Apply)
     * Receives input mapping [obsChannel][pluginChannel] and output mapping [pluginChannel][obsChannel]
     */
    std::function<void(const std::vector<std::vector<bool>>&, const std::vector<std::vector<bool>>&)>
        onObsMappingChanged;

    /**
     * Set callback to get current OBS channel mappings for Restore button
     * Returns pair of input mapping [obsChannel][pluginChannel] and output mapping [pluginChannel][obsChannel]
     */
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

        /**
         * Set fixed top rows that appear before subscribed channels.
         * These represent direct pass-through channels (e.g., OBS → Plugin).
         * @param names Channel names for the fixed rows
         * @param defaultEnabled If true, creates diagonal pattern by default
         */
        void setFixedTopRows(const juce::StringArray& names, bool defaultEnabled = true);

        /**
         * Get the mapping grid for fixed top rows only
         * @return 2D vector [fixedRow][clientChannel] = enabled
         */
        std::vector<std::vector<bool>> getFixedRowMappings() const;

        /**
         * Get the complete routing matrix including both fixed rows and device subscriptions
         * @return 2D vector [sourceChannel][pluginChannel] where sourceChannel = OBS channels + device subscriptions
         */
        std::vector<std::vector<bool>> getCompleteRoutingMatrix() const;

        /**
         * Set the mapping grid for fixed top rows
         * @param mappings 2D vector [fixedRow][clientChannel] = enabled
         */
        void setFixedRowMappings(const std::vector<std::vector<bool>>& mappings);

        /**
         * Set the complete routing matrix (all rows: fixed + subscriptions)
         * @param mappings 2D vector [row][clientChannel] = enabled
         */
        void setCompleteMatrix(const std::vector<std::vector<bool>>& mappings);

        /**
         * Reset fixed top rows to diagonal pattern (pass-through mapping)
         */
        void resetFixedRowsToDefault();

        /**
         * Clear all mappings for subscribed channel rows (but keep fixed rows intact)
         */
        void clearSubscribedRowMappings();

        std::vector<ChannelMapping> getInputMappings() const;
        std::vector<ChannelMapping> getOutputMappings() const;
        void setMappings(
            const std::vector<ChannelMapping>& inputMappings,
            const std::vector<ChannelMapping>& outputMappings
        );

        /**
         * Get the list of subscribed channels
         * @return Vector of subscribed channel rows
         */
        const std::vector<MappingRow>& getSubscribedChannels() const
        {
            return subscribedChannels;
        }

    private:
        juce::TableListBox table;
        std::vector<MappingRow> subscribedChannels;
        std::vector<std::vector<bool>> mappingGrid; // [row][col] = mapped
        int numClientChannels = 0;
        int numFixedTopRows = 0; // Number of fixed rows at top (e.g., OBS channels)
        juce::StringArray clientChannelNames;
        juce::StringArray fixedRowNames; // Names for fixed top rows
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
