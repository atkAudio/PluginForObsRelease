#include "AudioServerSettingsComponent.h"
#include <atkaudio/atkaudio.h>

namespace atk
{

//==============================================================================
// DeviceChannelTreeItem Implementation
//==============================================================================

AudioServerSettingsComponent::DeviceChannelTreeItem::DeviceChannelTreeItem(
    const juce::String& name,
    ItemType type,
    const juce::String& device,
    int channel,
    bool input
)
    : itemName(name)
    , itemType(type)
    , deviceName(device)
    , channelIndex(channel)
    , isInput(input)
{
}

bool AudioServerSettingsComponent::DeviceChannelTreeItem::mightContainSubItems()
{
    return itemType != ItemType::Channel;
}

void AudioServerSettingsComponent::DeviceChannelTreeItem::paintItem(juce::Graphics& g, int width, int height)
{
    // Use default TreeView colors
    auto& lf = getOwnerView()->getLookAndFeel();

    if (isSelected())
        g.fillAll(lf.findColour(juce::TreeView::selectedItemBackgroundColourId));

    // Use default text color from Component
    g.setColour(lf.findColour(juce::Label::textColourId));
    g.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), height * 0.7f, juce::Font::plain));

    juce::String displayText = itemName;
    if (itemType == ItemType::Channel)
    {
        // Always show placeholder for consistent spacing
        if (subscribed)
            displayText = "[X] " + displayText;
        else
            displayText = "[ ] " + displayText;
    }

    g.drawText(displayText, 4, 0, width - 4, height, juce::Justification::centredLeft, true);
}

void AudioServerSettingsComponent::DeviceChannelTreeItem::itemClicked(const juce::MouseEvent& e)
{
    if (itemType == ItemType::Channel)
    {
        setSubscribed(!subscribed);
        repaintItem();

        // Notify settings component to update matrix
        if (settingsComponent)
            settingsComponent->updateMappingMatrix();
    }
    else if (itemType == ItemType::Device)
    {
        // When a device is clicked, show its settings
        if (settingsComponent)
            settingsComponent->updateDeviceSettings(deviceName);
    }
}

void AudioServerSettingsComponent::DeviceChannelTreeItem::itemOpennessChanged(bool isNowOpen)
{
    // Lazily load channels when a device item is opened
    if (isNowOpen && itemType == ItemType::Device && !childrenLoaded && server)
    {
        childrenLoaded = true;

        // Get both channel count and actual channel names from the device
        juce::StringArray channelNames = server->getDeviceChannelNames(deviceName, isInput);
        int numChannels = static_cast<int>(channelNames.size());

        for (int ch = 0; ch < numChannels; ++ch)
        {
            // Format: (N) Actual Channel Name
            juce::String displayName = "(" + juce::String(ch + 1) + ") " + channelNames[ch];

            auto* channelItem = new DeviceChannelTreeItem(
                displayName,
                ItemType::Channel,
                deviceName,
                ch, // Store as 0-indexed internally
                isInput
            );
            channelItem->setServerInstance(server);
            channelItem->setSettingsComponent(settingsComponent);
            addSubItem(channelItem);
        }
    }
}

std::unique_ptr<juce::Component> AudioServerSettingsComponent::DeviceChannelTreeItem::createItemComponent()
{
    return nullptr; // Could add checkboxes here if needed
}

void AudioServerSettingsComponent::DeviceChannelTreeItem::setSubscribed(bool shouldBeSubscribed)
{
    subscribed = shouldBeSubscribed;
}

//==============================================================================
// ChannelMappingMatrix Implementation
//==============================================================================

AudioServerSettingsComponent::ChannelMappingMatrix::ChannelMappingMatrix()
{
    addAndMakeVisible(table);
    table.setModel(this);
    table.setMultipleSelectionEnabled(false);
    table.setClickingTogglesRowSelection(false);

    // Configure the header
    auto& header = table.getHeader();
    header.setVisible(true);

    // Add first column for row labels (will be set later via setFirstColumnName)
    // Width adjusted so first column + 4 channel columns fit in viewport (200 + 4*40 = 360px)
    header.addColumn("Routing", 1, 200, 150, 300, juce::TableHeaderComponent::notSortable);
}

int AudioServerSettingsComponent::ChannelMappingMatrix::getNumRows()
{
    return numFixedTopRows + static_cast<int>(subscribedChannels.size());
}

void AudioServerSettingsComponent::ChannelMappingMatrix::paintRowBackground(
    juce::Graphics& g,
    int rowNumber,
    int width,
    int height,
    bool rowIsSelected
)
{
    // Use JUCE default row background (no custom coloring)
}

void AudioServerSettingsComponent::ChannelMappingMatrix::paintCell(
    juce::Graphics& g,
    int rowNumber,
    int columnId,
    int width,
    int height,
    bool rowIsSelected
)
{
    int totalRows = numFixedTopRows + subscribedChannels.size();
    if (rowNumber >= totalRows)
        return;

    if (columnId == 1) // Device channel label column
    {
        g.setColour(juce::Colours::white);
        g.setFont(11.0f);

        juce::String label;
        if (rowNumber < numFixedTopRows)
        {
            // Fixed top row (OBS channel)
            label = fixedRowNames[rowNumber];
        }
        else
        {
            // Subscribed device channel
            const auto& channel = subscribedChannels[rowNumber - numFixedTopRows];
            label = channel.deviceName + " Ch " + juce::String(channel.deviceChannel + 1);
        }

        g.drawText(label, 4, 0, width - 8, height, juce::Justification::centredLeft, true);
    }
    else if (columnId >= 2) // Client channel mapping cells
    {
        int clientChannel = columnId - 2;
        if (clientChannel >= 0 && clientChannel < numClientChannels)
        {
            bool mapped = mappingGrid[rowNumber][clientChannel];

            // Draw "X" for mapped cells on normal background
            if (mapped)
            {
                g.setColour(juce::Colours::white);
                g.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 16.0f, juce::Font::bold));
                g.drawText("X", 0, 0, width, height, juce::Justification::centred);
            }

            // Draw cell border
            g.setColour(juce::Colours::black.withAlpha(0.3f));
            g.drawRect(0, 0, width, height, 1);
        }
    }
}

void AudioServerSettingsComponent::ChannelMappingMatrix::cellClicked(
    int rowNumber,
    int columnId,
    const juce::MouseEvent& e
)
{
    if (columnId >= 2) // Only client channel cells are clickable
    {
        int clientChannel = columnId - 2;
        int totalRows = numFixedTopRows + subscribedChannels.size();

        if (rowNumber >= 0 && rowNumber < totalRows && clientChannel >= 0 && clientChannel < numClientChannels)
        {
            mappingGrid[rowNumber][clientChannel] = !mappingGrid[rowNumber][clientChannel];
            table.repaintRow(rowNumber);
        }
    }
}

juce::String AudioServerSettingsComponent::ChannelMappingMatrix::getCellTooltip(int rowNumber, int columnId)
{
    if (rowNumber >= static_cast<int>(subscribedChannels.size()))
        return {};

    if (columnId == 1)
        return "Device channel: " + subscribedChannels[rowNumber].deviceName;
    else if (columnId >= 2)
    {
        int clientChannel = columnId - 2;
        return "Click to toggle routing to client channel " + juce::String(clientChannel + 1);
    }

    return {};
}

void AudioServerSettingsComponent::ChannelMappingMatrix::resized()
{
    table.setBounds(getLocalBounds());
}

void AudioServerSettingsComponent::ChannelMappingMatrix::setSubscribedChannels(const std::vector<MappingRow>& rows)
{
    // Save ALL current row mappings before resizing
    std::vector<std::vector<bool>> savedMappings = mappingGrid;
    std::vector<MappingRow> oldSubscribedChannels = subscribedChannels;

    subscribedChannels = rows;

    // Resize grid to include fixed top rows + subscribed rows
    int totalRows = numFixedTopRows + rows.size();
    mappingGrid.clear();
    mappingGrid.resize(totalRows, std::vector<bool>(numClientChannels, false));

    // Restore saved mappings for all rows that existed before
    int rowsToRestore = juce::jmin((int)savedMappings.size(), totalRows);
    for (int row = 0; row < rowsToRestore; ++row)
        for (int col = 0; col < numClientChannels && col < (int)savedMappings[row].size(); ++col)
            mappingGrid[row][col] = savedMappings[row][col];

    table.updateContent();
    table.repaint();
}

void AudioServerSettingsComponent::ChannelMappingMatrix::setNumClientChannels(int numChannels)
{
    numClientChannels = numChannels;

    // Generate default channel names
    clientChannelNames.clear();
    for (int i = 0; i < numChannels; ++i)
        clientChannelNames.add("Ch " + juce::String(i + 1));

    // Resize grid
    for (auto& row : mappingGrid)
        row.resize(numClientChannels, false);

    // Remove old client channel columns (keep column 1 which is the device label)
    while (table.getHeader().getNumColumns(true) > 1)
        table.getHeader().removeColumn(table.getHeader().getColumnIdOfIndex(1, true));

    // Add client channel columns
    for (int i = 0; i < numClientChannels; ++i)
    {
        table.getHeader().addColumn(
            clientChannelNames[i],
            i + 2, // columnId starts at 2 (1 is device label)
            40,    // width
            40,    // minimum width
            80,    // maximum width
            juce::TableHeaderComponent::notSortable
        );
    }

    DBG("Total columns after: " << table.getHeader().getNumColumns(true));

    table.updateContent();
    table.repaint();
}

void AudioServerSettingsComponent::ChannelMappingMatrix::setClientChannelNames(const juce::StringArray& names)
{
    clientChannelNames = names;
    numClientChannels = names.size();

    // Resize grid
    for (auto& row : mappingGrid)
        row.resize(numClientChannels, false);

    // Remove old client channel columns (keep column 1 which is the device label)
    while (table.getHeader().getNumColumns(true) > 1)
        table.getHeader().removeColumn(table.getHeader().getColumnIdOfIndex(1, true));

    // Add client channel columns with custom names
    for (int i = 0; i < numClientChannels; ++i)
    {
        table.getHeader().addColumn(
            clientChannelNames[i],
            i + 2, // columnId starts at 2 (1 is device label)
            40,    // width
            40,    // minimum width
            80,    // maximum width
            juce::TableHeaderComponent::notSortable
        );
    }

    table.updateContent();
    table.repaint();
}

void AudioServerSettingsComponent::ChannelMappingMatrix::setFirstColumnName(const juce::String& name)
{
    firstColumnName = name;

    // Update the first column header
    auto& header = table.getHeader();
    if (header.getNumColumns(true) > 0)
    {
        header.removeColumn(1);
        header.addColumn(name, 1, 200, 150, 300, juce::TableHeaderComponent::notSortable, 0);
    }
}

void AudioServerSettingsComponent::ChannelMappingMatrix::setFixedTopRows(
    const juce::StringArray& names,
    bool defaultEnabled
)
{
    fixedRowNames = names;
    numFixedTopRows = names.size();

    // Resize grid to include fixed rows + existing subscribed rows
    int totalRows = numFixedTopRows + subscribedChannels.size();
    mappingGrid.resize(totalRows);
    for (int row = 0; row < totalRows; ++row)
        mappingGrid[row].resize(numClientChannels, false);

    // Set diagonal pattern for fixed rows if enabled
    if (defaultEnabled)
        for (int row = 0; row < numFixedTopRows && row < numClientChannels; ++row)
            mappingGrid[row][row] = true;

    table.updateContent();
    table.repaint();
}

std::vector<std::vector<bool>> AudioServerSettingsComponent::ChannelMappingMatrix::getFixedRowMappings() const
{
    std::vector<std::vector<bool>> fixedMappings;
    for (int row = 0; row < numFixedTopRows && row < (int)mappingGrid.size(); ++row)
        fixedMappings.push_back(mappingGrid[row]);
    return fixedMappings;
}

std::vector<std::vector<bool>> AudioServerSettingsComponent::ChannelMappingMatrix::getCompleteRoutingMatrix() const
{
    // Return the complete mappingGrid (OBS channels + device subscriptions)
    return mappingGrid;
}

void AudioServerSettingsComponent::ChannelMappingMatrix::setFixedRowMappings(
    const std::vector<std::vector<bool>>& mappings
)
{
    // Apply the provided mappings to fixed rows
    for (size_t row = 0; row < mappings.size() && row < (size_t)numFixedTopRows && row < mappingGrid.size(); ++row)
        for (size_t col = 0; col < mappings[row].size() && col < (size_t)numClientChannels; ++col)
            mappingGrid[row][col] = mappings[row][col];

    table.updateContent();
    table.repaint();
}

void AudioServerSettingsComponent::ChannelMappingMatrix::setCompleteMatrix(
    const std::vector<std::vector<bool>>& mappings
)
{
    // Apply the complete routing matrix (OBS + device subscription rows)
    for (size_t row = 0; row < mappings.size() && row < mappingGrid.size(); ++row)
        for (size_t col = 0; col < mappings[row].size() && col < (size_t)numClientChannels; ++col)
            mappingGrid[row][col] = mappings[row][col];

    table.updateContent();
    table.repaint();
}

void AudioServerSettingsComponent::ChannelMappingMatrix::resetFixedRowsToDefault()
{
    // Reset fixed rows to diagonal pattern
    for (int row = 0; row < numFixedTopRows && row < (int)mappingGrid.size(); ++row)
        for (int col = 0; col < numClientChannels; ++col)
            mappingGrid[row][col] = (row == col);

    table.updateContent();
    table.repaint();
}

void AudioServerSettingsComponent::ChannelMappingMatrix::clearSubscribedRowMappings()
{
    // Remove all subscribed channel rows (keep only the fixed top rows)
    subscribedChannels.clear();

    // Resize mapping grid to only include fixed top rows
    mappingGrid.resize(numFixedTopRows);
    for (int row = 0; row < numFixedTopRows; ++row)
        mappingGrid[row].resize(numClientChannels);

    table.updateContent();
    table.repaint();
}

std::vector<ChannelMapping> AudioServerSettingsComponent::ChannelMappingMatrix::getInputMappings() const
{
    std::vector<ChannelMapping> mappings;

    for (size_t row = 0; row < subscribedChannels.size(); ++row)
    {
        if (!subscribedChannels[row].isInput)
            continue;

        for (int col = 0; col < numClientChannels; ++col)
        {
            // mappingGrid includes fixed top rows, so offset by numFixedTopRows
            if (mappingGrid[numFixedTopRows + row][col])
            {
                ChannelMapping mapping;
                mapping.deviceChannel.deviceName = subscribedChannels[row].deviceName;
                mapping.deviceChannel.deviceType = subscribedChannels[row].deviceType;
                mapping.deviceChannel.channelIndex = subscribedChannels[row].deviceChannel;
                mapping.deviceChannel.isInput = true;
                mapping.clientChannel = col;
                mappings.push_back(mapping);
            }
        }
    }

    return mappings;
}

std::vector<ChannelMapping> AudioServerSettingsComponent::ChannelMappingMatrix::getOutputMappings() const
{
    std::vector<ChannelMapping> mappings;

    for (size_t row = 0; row < subscribedChannels.size(); ++row)
    {
        if (subscribedChannels[row].isInput)
            continue;

        for (int col = 0; col < numClientChannels; ++col)
        {
            // mappingGrid includes fixed top rows, so offset by numFixedTopRows
            if (mappingGrid[numFixedTopRows + row][col])
            {
                ChannelMapping mapping;
                mapping.deviceChannel.deviceName = subscribedChannels[row].deviceName;
                mapping.deviceChannel.deviceType = subscribedChannels[row].deviceType;
                mapping.deviceChannel.channelIndex = subscribedChannels[row].deviceChannel;
                mapping.deviceChannel.isInput = false;
                mapping.clientChannel = col;
                mappings.push_back(mapping);
            }
        }
    }

    return mappings;
}

void AudioServerSettingsComponent::ChannelMappingMatrix::setMappings(
    const std::vector<ChannelMapping>& inputMappings,
    const std::vector<ChannelMapping>& outputMappings
)
{
    // Clear all mappings (will be restored below, including fixed rows via setObsChannelMappings)
    for (auto& row : mappingGrid)
        std::fill(row.begin(), row.end(), false);

    // Apply input mappings (only for subscribed device channels, not fixed rows)
    for (const auto& mapping : inputMappings)
    {
        // Find which row this mapping corresponds to in subscribedChannels
        for (size_t row = 0; row < subscribedChannels.size(); ++row)
        {
            if (subscribedChannels[row].isInput
                && subscribedChannels[row].deviceName == mapping.deviceChannel.deviceName
                && subscribedChannels[row].deviceChannel == mapping.deviceChannel.channelIndex)
            {
                if (mapping.clientChannel < numClientChannels)
                {
                    // subscribedChannels rows start after fixed top rows in mappingGrid
                    mappingGrid[numFixedTopRows + row][mapping.clientChannel] = true;
                }
            }
        }
    }

    // Apply output mappings (only for subscribed device channels, not fixed rows)
    for (const auto& mapping : outputMappings)
    {
        // Find which row this mapping corresponds to in subscribedChannels
        for (size_t row = 0; row < subscribedChannels.size(); ++row)
        {
            if (!subscribedChannels[row].isInput
                && subscribedChannels[row].deviceName == mapping.deviceChannel.deviceName
                && subscribedChannels[row].deviceChannel == mapping.deviceChannel.channelIndex)
            {
                if (mapping.clientChannel < numClientChannels)
                {
                    // subscribedChannels rows start after fixed top rows in mappingGrid
                    mappingGrid[numFixedTopRows + row][mapping.clientChannel] = true;
                }
            }
        }
    }

    repaint();
}

//==============================================================================
// AudioServerSettingsComponent Implementation
//==============================================================================

AudioServerSettingsComponent::AudioServerSettingsComponent(AudioClient* audioClient, int clientChannels)
    : client(audioClient)
    , server(AudioServer::getInstance())
{
    // Input tree
    inputTreeLabel.setText("Input", juce::dontSendNotification);
    inputTreeLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    addAndMakeVisible(inputTreeLabel);

    inputTreeView = std::make_unique<juce::TreeView>();
    inputRootItem = std::make_unique<DeviceChannelTreeItem>("Inputs", DeviceChannelTreeItem::ItemType::DeviceType);
    inputTreeView->setRootItem(inputRootItem.get());
    inputTreeView->setRootItemVisible(false);
    inputTreeView->setDefaultOpenness(false); // Control openness explicitly per item
    inputTreeView->setColour(juce::TreeView::backgroundColourId, findColour(juce::ResizableWindow::backgroundColourId));
    inputTreeView->setColour(juce::TreeView::linesColourId, juce::Colours::grey);
    addAndMakeVisible(inputTreeView.get());

    // Output tree
    outputTreeLabel.setText("Output", juce::dontSendNotification);
    outputTreeLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    addAndMakeVisible(outputTreeLabel);

    outputTreeView = std::make_unique<juce::TreeView>();
    outputRootItem = std::make_unique<DeviceChannelTreeItem>("Outputs", DeviceChannelTreeItem::ItemType::DeviceType);
    outputTreeView->setRootItem(outputRootItem.get());
    outputTreeView->setRootItemVisible(false);
    outputTreeView->setDefaultOpenness(false); // Control openness explicitly per item
    outputTreeView->setColour(
        juce::TreeView::backgroundColourId,
        findColour(juce::ResizableWindow::backgroundColourId)
    );
    outputTreeView->setColour(juce::TreeView::linesColourId, juce::Colours::grey);
    addAndMakeVisible(outputTreeView.get());

    // Use provided channel count
    if (clientChannels <= 0)
        clientChannels = 2; // Default fallback

    DBG("AudioServerSettingsComponent: Client is " << (client ? "valid" : "null"));
    DBG("AudioServerSettingsComponent: Client has " << clientChannels << " channels");

    inputMappingMatrix = std::make_unique<ChannelMappingMatrix>();
    addAndMakeVisible(inputMappingMatrix.get());
    inputMappingMatrix->setFirstColumnName("Routing");
    inputMappingMatrix->setNumClientChannels(clientChannels);

    outputMappingMatrix = std::make_unique<ChannelMappingMatrix>();
    addAndMakeVisible(outputMappingMatrix.get());
    outputMappingMatrix->setFirstColumnName("Routing");
    outputMappingMatrix->setNumClientChannels(clientChannels);

    // Buttons
    applyButton.addListener(this);
    addAndMakeVisible(applyButton);

    restoreButton.addListener(this);
    addAndMakeVisible(restoreButton);

    cancelButton.addListener(this);
    addAndMakeVisible(cancelButton);

    deviceButton.addListener(this);
    addAndMakeVisible(deviceButton);

    // Build device trees immediately
    // Note: Device enumeration can be slow with some audio drivers, but deferring
    // causes timing issues with state restoration. Build synchronously.
    updateDeviceTrees();

    // Restore current subscriptions from the client after devices are enumerated
    if (client)
    {
        auto currentState = client->getSubscriptions();
        setSubscriptionState(currentState, true);

        // Now update the mapping matrix to reflect the subscriptions
        // This must happen AFTER setSubscriptionState marks the tree checkboxes
        updateMappingMatrix();
    }

    // Start timer for periodic updates
    startTimer(1000);
}

AudioServerSettingsComponent::~AudioServerSettingsComponent()
{
    // Close device settings dialog if open
    if (deviceSettingsDialog != nullptr)
        deviceSettingsDialog->exitModalState(0);

    applyButton.removeListener(this);
    restoreButton.removeListener(this);
    cancelButton.removeListener(this);
    stopTimer();
    inputTreeView->setRootItem(nullptr);
    outputTreeView->setRootItem(nullptr);
}

void AudioServerSettingsComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioServerSettingsComponent::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    // Bottom buttons (right to left: Reset, Restore, Apply, Device...)
    auto buttonArea = bounds.removeFromBottom(30);
    buttonArea.removeFromTop(5); // Gap
    applyButton.setBounds(buttonArea.removeFromRight(80));
    buttonArea.removeFromRight(5); // Gap
    restoreButton.setBounds(buttonArea.removeFromRight(80));
    buttonArea.removeFromRight(5); // Gap
    cancelButton.setBounds(buttonArea.removeFromRight(80));
    buttonArea.removeFromRight(5); // Gap
    deviceButton.setBounds(buttonArea.removeFromRight(80));

    bounds.removeFromBottom(10); // Gap

    // Top section with trees (60% of height)
    auto topSection = bounds.removeFromTop(static_cast<int>(bounds.getHeight() * 0.6f));

    // Split top section vertically for input/output trees
    auto inputSection = topSection.removeFromLeft(topSection.getWidth() / 2).reduced(5);
    auto outputSection = topSection.reduced(5);

    inputTreeLabel.setBounds(inputSection.removeFromTop(30));
    inputTreeView->setBounds(inputSection);

    outputTreeLabel.setBounds(outputSection.removeFromTop(30));
    outputTreeView->setBounds(outputSection);

    // Bottom section with matrices (40% of height), split horizontally
    bounds.removeFromTop(10); // Gap

    auto matrixSection = bounds;
    auto leftMatrixSection = matrixSection.removeFromLeft(matrixSection.getWidth() / 2).reduced(5);
    auto rightMatrixSection = matrixSection.reduced(5);

    // Input matrix on the left
    inputMappingMatrix->setBounds(leftMatrixSection);

    // Output matrix on the right
    outputMappingMatrix->setBounds(rightMatrixSection);
}

void AudioServerSettingsComponent::updateDeviceTrees()
{
    if (!server)
        return;

    // Clear existing items
    inputRootItem->clearSubItems();
    outputRootItem->clearSubItems();

    // Helper to sort device types: ASIO first, then Windows Audio/WASAPI, then rest
    auto sortDeviceTypes = [](const std::map<juce::String, juce::StringArray>& devicesByType)
    {
        std::vector<std::pair<juce::String, juce::StringArray>> sorted;
        std::vector<std::pair<juce::String, juce::StringArray>> asio;
        std::vector<std::pair<juce::String, juce::StringArray>> windowsAudio;
        std::vector<std::pair<juce::String, juce::StringArray>> rest;

        for (const auto& [typeName, devices] : devicesByType)
            if (typeName.containsIgnoreCase("ASIO"))
                asio.push_back({typeName, devices});
            else if (typeName.containsIgnoreCase("Windows Audio") || typeName.containsIgnoreCase("WASAPI"))
                windowsAudio.push_back({typeName, devices});
            else
                rest.push_back({typeName, devices});

        // Combine: ASIO, then Windows Audio, then rest
        sorted.insert(sorted.end(), asio.begin(), asio.end());
        sorted.insert(sorted.end(), windowsAudio.begin(), windowsAudio.end());
        sorted.insert(sorted.end(), rest.begin(), rest.end());

        return sorted;
    };

    // Populate input devices grouped by type (ASIO, WASAPI, etc.)
    auto inputDevicesByType = server->getInputDevicesByType();
    auto sortedInputTypes = sortDeviceTypes(inputDevicesByType);

    for (const auto& [typeName, devices] : sortedInputTypes)
    {
        // Create device type category (e.g., "ASIO", "WASAPI")
        auto* typeItem = new DeviceChannelTreeItem(typeName, DeviceChannelTreeItem::ItemType::DeviceType, {}, -1, true);
        typeItem->setServerInstance(server);
        typeItem->setSettingsComponent(this);
        inputRootItem->addSubItem(typeItem);

        // Add devices under this type
        for (const auto& deviceName : devices)
        {
            auto* deviceItem =
                new DeviceChannelTreeItem(deviceName, DeviceChannelTreeItem::ItemType::Device, deviceName, -1, true);
            deviceItem->setServerInstance(server);
            deviceItem->setSettingsComponent(this);
            typeItem->addSubItem(deviceItem);

            // Don't create channels yet - they will be loaded lazily when device is opened
            // Don't open device items - keep them collapsed
        }

        // Keep type category open to show device names
        typeItem->setOpen(true);
    }

    // Populate output devices grouped by type (ASIO, WASAPI, etc.)
    auto outputDevicesByType = server->getOutputDevicesByType();
    auto sortedOutputTypes = sortDeviceTypes(outputDevicesByType);

    for (const auto& [typeName, devices] : sortedOutputTypes)
    {
        // Create device type category (e.g., "ASIO", "WASAPI")
        auto* typeItem =
            new DeviceChannelTreeItem(typeName, DeviceChannelTreeItem::ItemType::DeviceType, {}, -1, false);
        typeItem->setServerInstance(server);
        typeItem->setSettingsComponent(this);
        outputRootItem->addSubItem(typeItem);

        // Add devices under this type
        for (const auto& deviceName : devices)
        {
            auto* deviceItem =
                new DeviceChannelTreeItem(deviceName, DeviceChannelTreeItem::ItemType::Device, deviceName, -1, false);
            deviceItem->setServerInstance(server);
            deviceItem->setSettingsComponent(this);
            typeItem->addSubItem(deviceItem);

            // Don't create channels yet - they will be loaded lazily when device is opened
            // Don't open device items - keep them collapsed
        }

        // Keep type category open to show device names
        typeItem->setOpen(true);
    }

    // Restore subscriptions from client state (but don't expand tree on initial load)
    if (client)
    {
        auto state = client->getSubscriptions();
        setSubscriptionState(state, false); // Don't expand - keep devices collapsed
    }
}

void AudioServerSettingsComponent::updateMappingMatrix()
{
    std::vector<ChannelMappingMatrix::MappingRow> inputRows;
    std::vector<ChannelMappingMatrix::MappingRow> outputRows;

    // Collect subscribed channels from input tree
    // Structure: Root > DeviceType > Device > Channel
    for (int i = 0; i < inputRootItem->getNumSubItems(); ++i)
    {
        auto* typeItem = dynamic_cast<DeviceChannelTreeItem*>(inputRootItem->getSubItem(i));
        if (!typeItem || typeItem->getType() != DeviceChannelTreeItem::ItemType::DeviceType)
            continue;

        juce::String deviceType = typeItem->getItemName(); // Get device type (e.g., "ASIO", "WASAPI")

        for (int j = 0; j < typeItem->getNumSubItems(); ++j)
        {
            auto* deviceItem = dynamic_cast<DeviceChannelTreeItem*>(typeItem->getSubItem(j));
            if (!deviceItem || deviceItem->getType() != DeviceChannelTreeItem::ItemType::Device)
                continue;

            for (int k = 0; k < deviceItem->getNumSubItems(); ++k)
            {
                auto* channelItem = dynamic_cast<DeviceChannelTreeItem*>(deviceItem->getSubItem(k));
                if (channelItem && channelItem->isSubscribed())
                {
                    ChannelMappingMatrix::MappingRow row;
                    row.deviceName = channelItem->getDeviceName();
                    row.deviceType = deviceType;
                    row.deviceChannel = channelItem->getChannelIndex();
                    row.isInput = true;
                    inputRows.push_back(row);
                }
            }
        }
    }

    // Collect subscribed channels from output tree
    // Structure: Root > DeviceType > Device > Channel
    for (int i = 0; i < outputRootItem->getNumSubItems(); ++i)
    {
        auto* typeItem = dynamic_cast<DeviceChannelTreeItem*>(outputRootItem->getSubItem(i));
        if (!typeItem || typeItem->getType() != DeviceChannelTreeItem::ItemType::DeviceType)
            continue;

        juce::String deviceType = typeItem->getItemName(); // Get device type (e.g., "ASIO", "WASAPI")

        for (int j = 0; j < typeItem->getNumSubItems(); ++j)
        {
            auto* deviceItem = dynamic_cast<DeviceChannelTreeItem*>(typeItem->getSubItem(j));
            if (!deviceItem || deviceItem->getType() != DeviceChannelTreeItem::ItemType::Device)
                continue;

            for (int k = 0; k < deviceItem->getNumSubItems(); ++k)
            {
                auto* channelItem = dynamic_cast<DeviceChannelTreeItem*>(deviceItem->getSubItem(k));
                if (channelItem && channelItem->isSubscribed())
                {
                    ChannelMappingMatrix::MappingRow row;
                    row.deviceName = channelItem->getDeviceName();
                    row.deviceType = deviceType;
                    row.deviceChannel = channelItem->getChannelIndex();
                    row.isInput = false;
                    outputRows.push_back(row);
                }
            }
        }
    }

    inputMappingMatrix->setSubscribedChannels(inputRows);
    outputMappingMatrix->setSubscribedChannels(outputRows);
}

AudioClientState AudioServerSettingsComponent::getSubscriptionState() const
{
    AudioClientState state;

    // Get list of subscribed channels (no clientChannel mapping - just subscriptions)
    if (inputMappingMatrix)
    {
        for (const auto& sub : inputMappingMatrix->getSubscribedChannels())
        {
            if (sub.isInput)
            {
                ChannelSubscription subscription;
                subscription.deviceName = sub.deviceName;
                subscription.deviceType = sub.deviceType;
                subscription.channelIndex = sub.deviceChannel;
                subscription.isInput = true;
                state.inputSubscriptions.push_back(subscription);
            }
        }
    }

    if (outputMappingMatrix)
    {
        for (const auto& sub : outputMappingMatrix->getSubscribedChannels())
        {
            if (!sub.isInput)
            {
                ChannelSubscription subscription;
                subscription.deviceName = sub.deviceName;
                subscription.deviceType = sub.deviceType;
                subscription.channelIndex = sub.deviceChannel;
                subscription.isInput = false;
                state.outputSubscriptions.push_back(subscription);
            }
        }
    }

    return state;
}

void AudioServerSettingsComponent::setSubscriptionState(const AudioClientState& state, bool expandToSubscriptions)
{
    // Mark channels as subscribed in trees and optionally expand to show them
    // Structure: Root > DeviceType > Device > Channel
    auto markSubscribedAndExpand =
        [expandToSubscriptions](DeviceChannelTreeItem* root, const std::vector<ChannelSubscription>& subscriptions)
    {
        for (int i = 0; i < root->getNumSubItems(); ++i)
        {
            auto* typeItem = dynamic_cast<DeviceChannelTreeItem*>(root->getSubItem(i));
            if (!typeItem || typeItem->getType() != DeviceChannelTreeItem::ItemType::DeviceType)
                continue;

            juce::String deviceType = typeItem->getItemName();
            bool typeHasSubscriptions = false;

            for (int j = 0; j < typeItem->getNumSubItems(); ++j)
            {
                auto* deviceItem = dynamic_cast<DeviceChannelTreeItem*>(typeItem->getSubItem(j));
                if (!deviceItem || deviceItem->getType() != DeviceChannelTreeItem::ItemType::Device)
                    continue;

                bool deviceHasSubscriptions = false;

                // Only trigger lazy loading if we need to expand
                if (expandToSubscriptions && deviceItem->getNumSubItems() == 0)
                {
                    // Check if this device has any subscriptions before expanding
                    for (const auto& sub : subscriptions)
                    {
                        if (sub.deviceType == deviceType && sub.deviceName == deviceItem->getDeviceName())
                        {
                            deviceItem->setOpen(true); // Trigger lazy load
                            break;
                        }
                    }
                }

                for (int k = 0; k < deviceItem->getNumSubItems(); ++k)
                {
                    auto* channelItem = dynamic_cast<DeviceChannelTreeItem*>(deviceItem->getSubItem(k));
                    if (!channelItem || channelItem->getType() != DeviceChannelTreeItem::ItemType::Channel)
                        continue;

                    // Check if this channel is in the subscriptions
                    bool subscribed = false;
                    for (const auto& sub : subscriptions)
                    {
                        if (sub.deviceType == deviceType
                            && sub.deviceName == channelItem->getDeviceName()
                            && sub.channelIndex == channelItem->getChannelIndex())
                        {
                            subscribed = true;
                            deviceHasSubscriptions = true;
                            typeHasSubscriptions = true;
                            break;
                        }
                    }
                    channelItem->setSubscribed(subscribed);
                }

                // Expand device if it has subscriptions and we want to expand
                if (expandToSubscriptions && deviceHasSubscriptions)
                    deviceItem->setOpen(true);
            }

            // Keep type item open (it should already be open)
            if (typeHasSubscriptions)
                typeItem->setOpen(true);
        }
    };

    markSubscribedAndExpand(inputRootItem.get(), state.inputSubscriptions);
    markSubscribedAndExpand(outputRootItem.get(), state.outputSubscriptions);

    // Don't call updateMappingMatrix() here - subscriptions are already set in constructor
    // and routing matrix has been restored. This just marks checkboxes in the tree.
    // updateMappingMatrix() should only be called when user actually changes subscriptions.

    // Repaint trees
    inputTreeView->repaint();
    outputTreeView->repaint();
}

void AudioServerSettingsComponent::applySubscriptions()
{
    if (!client)
        return;

    auto state = getSubscriptionState();
    client->setSubscriptions(state);

    // Notify callback about complete routing matrix changes (OBS + device subscriptions)
    if (onObsMappingChanged)
    {
        auto matrices = getCompleteRoutingMatrices();
        onObsMappingChanged(matrices.first, matrices.second);
    }

    DBG("AudioServer: Applied subscriptions - "
        << state.inputSubscriptions.size()
        << " input, "
        << state.outputSubscriptions.size()
        << " output");
}

void AudioServerSettingsComponent::setClientChannelInfo(
    const juce::StringArray& channelNames,
    const juce::String& firstColumnName
)
{
    // Set the same channel info for both input and output matrices
    setClientChannelInfo(channelNames, channelNames, firstColumnName);
}

void AudioServerSettingsComponent::setClientChannelInfo(
    const juce::StringArray& inputChannelNames,
    const juce::StringArray& outputChannelNames,
    const juce::String& firstColumnName
)
{
    if (inputMappingMatrix)
    {
        inputMappingMatrix->setClientChannelNames(inputChannelNames);
        inputMappingMatrix->setFirstColumnName(firstColumnName);
    }

    if (outputMappingMatrix)
    {
        outputMappingMatrix->setClientChannelNames(outputChannelNames);
        outputMappingMatrix->setFirstColumnName(firstColumnName);
    }

    // Don't call updateMappingMatrix() here - it rebuilds subscriptions from tree
    // which may not be loaded yet. Subscriptions are set up in constructor
    // and updateMappingMatrix() is called when user actually changes subscriptions via tree clicks.
}

void AudioServerSettingsComponent::setInputFixedTopRows(const juce::StringArray& names, bool defaultEnabled)
{
    if (inputMappingMatrix)
        inputMappingMatrix->setFixedTopRows(names, defaultEnabled);
}

void AudioServerSettingsComponent::setOutputFixedTopRows(const juce::StringArray& names, bool defaultEnabled)
{
    if (outputMappingMatrix)
        outputMappingMatrix->setFixedTopRows(names, defaultEnabled);
}

std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>>
AudioServerSettingsComponent::getObsChannelMappings() const
{
    std::vector<std::vector<bool>> inputMapping;
    std::vector<std::vector<bool>> outputMapping;

    if (inputMappingMatrix)
        inputMapping = inputMappingMatrix->getFixedRowMappings();
    if (outputMappingMatrix)
        outputMapping = outputMappingMatrix->getFixedRowMappings();

    return {inputMapping, outputMapping};
}

std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<bool>>>
AudioServerSettingsComponent::getCompleteRoutingMatrices() const
{
    std::vector<std::vector<bool>> inputMatrix;
    std::vector<std::vector<bool>> outputMatrix;

    if (inputMappingMatrix)
        inputMatrix = inputMappingMatrix->getCompleteRoutingMatrix();
    if (outputMappingMatrix)
        outputMatrix = outputMappingMatrix->getCompleteRoutingMatrix();

    return {inputMatrix, outputMatrix};
}

void AudioServerSettingsComponent::setObsChannelMappings(
    const std::vector<std::vector<bool>>& inputMapping,
    const std::vector<std::vector<bool>>& outputMapping
)
{
    if (inputMappingMatrix)
        inputMappingMatrix->setFixedRowMappings(inputMapping);
    if (outputMappingMatrix)
        outputMappingMatrix->setFixedRowMappings(outputMapping);
}

void AudioServerSettingsComponent::setCompleteRoutingMatrices(
    const std::vector<std::vector<bool>>& inputMapping,
    const std::vector<std::vector<bool>>& outputMapping
)
{
    if (inputMappingMatrix)
        inputMappingMatrix->setCompleteMatrix(inputMapping);
    if (outputMappingMatrix)
        outputMappingMatrix->setCompleteMatrix(outputMapping);
}

void AudioServerSettingsComponent::setClientChannelCount(int numChannels, const juce::String& firstColumnName)
{
    if (inputMappingMatrix)
    {
        inputMappingMatrix->setNumClientChannels(numChannels);
        inputMappingMatrix->setFirstColumnName(firstColumnName);
    }

    if (outputMappingMatrix)
    {
        outputMappingMatrix->setNumClientChannels(numChannels);
        outputMappingMatrix->setFirstColumnName(firstColumnName);
    }

    // Update the mapping matrix to reflect new channel count
    updateMappingMatrix();
}

void AudioServerSettingsComponent::buttonClicked(juce::Button* button)
{
    if (button == &applyButton)
    {
        applySubscriptions();
    }
    else if (button == &restoreButton)
    {
        // Restore visual state from current active configuration (no Apply)

        // Restore OBS channel mappings from processor if callback is set
        if (getCurrentObsMappings)
        {
            auto [inputMapping, outputMapping] = getCurrentObsMappings();
            setObsChannelMappings(inputMapping, outputMapping);
        }

        // Restore device subscriptions from client
        if (client)
        {
            auto state = client->getSubscriptions();
            setSubscriptionState(state);
        }
    }
    else if (button == &cancelButton)
    {
        // Show confirmation dialog
        auto options = juce::MessageBoxOptions()
                           .withIconType(juce::MessageBoxIconType::QuestionIcon)
                           .withTitle("Reset Channel Mapping")
                           .withMessage(
                               "Reset all channel mappings to default and clear all device subscriptions?\n\n"
                               "This will:\n"
                               "- Reset OBS channels to diagonal pass-through\n"
                               "- Clear all device channel subscriptions\n"
                               "- Apply changes immediately"
                           )
                           .withButton("Reset")
                           .withButton("Cancel");

        juce::AlertWindow::showAsync(
            options,
            [this](int result)
            {
                if (result == 1) // "Reset" button
                {
                    // Reset fixed top rows (OBS channels) to diagonal pattern
                    if (inputMappingMatrix)
                        inputMappingMatrix->resetFixedRowsToDefault();
                    if (outputMappingMatrix)
                        outputMappingMatrix->resetFixedRowsToDefault();

                    // Clear all device channel subscriptions from matrices
                    if (inputMappingMatrix)
                        inputMappingMatrix->clearSubscribedRowMappings();
                    if (outputMappingMatrix)
                        outputMappingMatrix->clearSubscribedRowMappings();

                    // Uncheck all device channels in the tree
                    clearAllDeviceSubscriptions(inputRootItem.get());
                    clearAllDeviceSubscriptions(outputRootItem.get());

                    // Refresh tree views
                    if (inputTreeView)
                        inputTreeView->repaint();
                    if (outputTreeView)
                        outputTreeView->repaint();

                    // Apply the reset immediately (sets empty subscriptions to client)
                    applySubscriptions();
                }
            }
        );
    }
    else if (button == &deviceButton)
    {
        showDeviceSettings();
    }
}

void AudioServerSettingsComponent::showDeviceSettings()
{
    // Use external device manager if set, otherwise show error
    if (!externalDeviceManager)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Device Settings",
            "Device configuration is not available.\n\n"
            "No device manager has been set for this component.",
            "OK"
        );
        return;
    }

    // Show standard JUCE AudioDeviceSelectorComponent
    // Hide channel selectors (only show device type and sample rate/buffer)
    auto* audioSettingsComp = new juce::AudioDeviceSelectorComponent(
        *externalDeviceManager,
        0,     // minAudioInputChannels
        0,     // maxAudioInputChannels - 0 hides input channels
        0,     // minAudioOutputChannels
        0,     // maxAudioOutputChannels - 0 hides output channels
        false, // showMidiInputOptions
        false, // showMidiOutputSelector
        false, // showChannelsAsStereoPairs
        false  // hideAdvancedOptionsWithButton
    );

    audioSettingsComp->setSize(500, 450);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(audioSettingsComp);
    o.dialogTitle = "Audio Device Settings";
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = false;

    deviceSettingsDialog = o.launchAsync();
}

void AudioServerSettingsComponent::clearAllDeviceSubscriptions(DeviceChannelTreeItem* root)
{
    if (!root)
        return;

    for (int i = 0; i < root->getNumSubItems(); ++i)
    {
        auto* typeItem = dynamic_cast<DeviceChannelTreeItem*>(root->getSubItem(i));
        if (!typeItem)
            continue;

        for (int j = 0; j < typeItem->getNumSubItems(); ++j)
        {
            auto* deviceItem = dynamic_cast<DeviceChannelTreeItem*>(typeItem->getSubItem(j));
            if (!deviceItem)
                continue;

            for (int k = 0; k < deviceItem->getNumSubItems(); ++k)
            {
                auto* channelItem = dynamic_cast<DeviceChannelTreeItem*>(deviceItem->getSubItem(k));
                if (channelItem)
                    channelItem->setSubscribed(false);
            }
        }
    }
}

void AudioServerSettingsComponent::timerCallback()
{
    // Refresh device tree nodes that are currently open
    // This ensures that if a device's channel count changes at runtime,
    // the tree view is updated to show the new channels

    auto refreshOpenDeviceNodes = [this](DeviceChannelTreeItem* root)
    {
        if (!root || !server)
            return;

        for (int i = 0; i < root->getNumSubItems(); ++i)
        {
            auto* typeItem = dynamic_cast<DeviceChannelTreeItem*>(root->getSubItem(i));
            if (!typeItem)
                continue;

            // Check each device under this type
            for (int j = 0; j < typeItem->getNumSubItems(); ++j)
            {
                auto* deviceItem = dynamic_cast<DeviceChannelTreeItem*>(typeItem->getSubItem(j));
                if (!deviceItem)
                    continue;

                // If device node is open, refresh its channels
                if (deviceItem->isOpen() && deviceItem->getDeviceName().isNotEmpty())
                {
                    juce::String deviceName = deviceItem->getDeviceName();
                    bool isInput = deviceItem->isInputDevice();

                    // Get current channel count
                    juce::StringArray channelNames = server->getDeviceChannelNames(deviceName, isInput);
                    int newChannelCount = static_cast<int>(channelNames.size());

                    // Only refresh if channel count changed
                    if (newChannelCount != deviceItem->getNumSubItems())
                    {
                        DBG("AudioServerSettingsComponent: Device '"
                            + deviceName
                            + "' channel count changed from "
                            + juce::String(deviceItem->getNumSubItems())
                            + " to "
                            + juce::String(newChannelCount)
                            + " - refreshing tree node");

                        // Clear and rebuild child items
                        deviceItem->clearSubItems();
                        deviceItem->resetChildrenLoadedFlag();

                        // Reload channels
                        for (int ch = 0; ch < newChannelCount; ++ch)
                        {
                            juce::String displayName = "(" + juce::String(ch + 1) + ") " + channelNames[ch];

                            auto* channelItem = new DeviceChannelTreeItem(
                                displayName,
                                DeviceChannelTreeItem::ItemType::Channel,
                                deviceName,
                                ch,
                                isInput
                            );
                            channelItem->setServerInstance(server);
                            channelItem->setSettingsComponent(this);
                            deviceItem->addSubItem(channelItem);
                        }

                        // Repaint the tree
                        deviceItem->treeHasChanged();
                    }
                }
            }
        }
    };

    // Refresh both input and output trees
    refreshOpenDeviceNodes(inputRootItem.get());
    refreshOpenDeviceNodes(outputRootItem.get());
}

void AudioServerSettingsComponent::updateDeviceSettings(const juce::String& deviceName)
{
    currentDeviceName = deviceName;
    // Sample rate and buffer size controls removed - these are managed by OBS
}

} // namespace atk
