#include "midi_to_obs_dialog.h"

#ifdef ENABLE_QT

#include <atkaudio/midi_obs_controller.h>

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <array>
#include <algorithm>

namespace
{
enum MidiToObsTableColumn
{
    midi_to_obs_column_enabled = 0,
    midi_to_obs_column_learn,
    midi_to_obs_column_device,
    midi_to_obs_column_type,
    midi_to_obs_column_channel,
    midi_to_obs_column_data1,
    midi_to_obs_column_mode,
    midi_to_obs_column_action,
    midi_to_obs_column_target,
    midi_to_obs_column_invert,
    midi_to_obs_column_min,
    midi_to_obs_column_max,
    midi_to_obs_column_count,
};

struct ObsSourceEntry
{
    juce::String uuid;
    juce::String name;
};

struct ObsSceneEntry
{
    juce::String uuid;
    juce::String name;
};

constexpr auto previousSceneTargetName = "Previous Scene";
constexpr auto nextSceneTargetName = "Next Scene";
constexpr auto transitionTbarTargetName = "TBar";
constexpr auto transitionTriggerTargetName = "Trigger";

QString toQString(juce::String text)
{
    return QString::fromUtf8(text.toRawUTF8());
}

juce::String toJuceString(QString text)
{
    return juce::String::fromUTF8(text.toUtf8().constData());
}

bool isSceneAction(int actionType)
{
    return actionType == atk::midi_obs_action_type_scene;
}

bool isTransitionAction(int actionType)
{
    return actionType == atk::midi_obs_action_type_transition;
}

std::vector<ObsSourceEntry> getObsSources()
{
    std::vector<ObsSourceEntry> sources;

    obs_enum_sources(
        [](void* context, obs_source_t* source)
        {
            auto* entries = static_cast<std::vector<ObsSourceEntry>*>(context);
            ObsSourceEntry entry;
            entry.uuid = juce::String(obs_source_get_uuid(source));
            entry.name = juce::String(obs_source_get_name(source));

            if (entry.name.isNotEmpty())
                entries->push_back(entry);

            return true;
        },
        &sources
    );

    std::sort(
        sources.begin(),
        sources.end(),
        [](const ObsSourceEntry& left, const ObsSourceEntry& right)
        { return left.name.compareIgnoreCase(right.name) < 0; }
    );

    return sources;
}

std::vector<ObsSceneEntry> getObsScenes()
{
    std::vector<ObsSceneEntry> scenes;

    obs_frontend_source_list sceneList = {};
    obs_frontend_get_scenes(&sceneList);

    scenes.reserve(sceneList.sources.num);

    for (size_t i = 0; i < sceneList.sources.num; ++i)
    {
        auto* source = sceneList.sources.array[i];
        if (source == nullptr)
            continue;

        ObsSceneEntry entry;
        entry.uuid = juce::String(obs_source_get_uuid(source));
        entry.name = juce::String(obs_source_get_name(source));

        if (entry.name.isNotEmpty())
            scenes.push_back(entry);
    }

    obs_frontend_source_list_free(&sceneList);

    std::sort(
        scenes.begin(),
        scenes.end(),
        [](const ObsSceneEntry& left, const ObsSceneEntry& right)
        { return left.name.compareIgnoreCase(right.name) < 0; }
    );

    return scenes;
}

void addMessageTypeOptions(QComboBox* combo)
{
    combo->addItem("CC", atk::midi_obs_message_type_cc);
    combo->addItem("Note", atk::midi_obs_message_type_note);
    combo->addItem("Program", atk::midi_obs_message_type_program_change);
}

void addModeOptions(QComboBox* combo)
{
    combo->addItem("Absolute", atk::midi_obs_interaction_mode_absolute);
    combo->addItem("Trigger", atk::midi_obs_interaction_mode_trigger);
    combo->addItem("Toggle", atk::midi_obs_interaction_mode_toggle);
}

void addActionOptions(QComboBox* combo)
{
    combo->addItem("Source Volume", atk::midi_obs_action_type_source_volume);
    combo->addItem("Source Mute", atk::midi_obs_action_type_source_mute);
    combo->addItem("Source Enabled", atk::midi_obs_action_type_source_enabled);
    combo->addItem("Media Play/Pause", atk::midi_obs_action_type_media_play_pause);
    combo->addItem("Media Restart", atk::midi_obs_action_type_media_restart);
    combo->addItem("Media Stop", atk::midi_obs_action_type_media_stop);
    combo->addItem("Scene", atk::midi_obs_action_type_scene);
    combo->addItem("Transition", atk::midi_obs_action_type_transition);
}

void syncMappingTargetFromCombo(atk::MidiObsMapping& mapping, QComboBox* combo)
{
    mapping.targetName = toJuceString(combo->currentText());

    if (isTransitionAction(mapping.actionType))
    {
        mapping.targetUuid.clear();
        return;
    }

    if (isSceneAction(mapping.actionType))
    {
        if (mapping.targetName == previousSceneTargetName || mapping.targetName == nextSceneTargetName)
            mapping.targetUuid.clear();
        else
            mapping.targetUuid = toJuceString(combo->currentData().toString());

        return;
    }

    mapping.targetUuid = toJuceString(combo->currentData().toString());
}

void populateTargetComboForMapping(
    QComboBox* combo,
    atk::MidiObsMapping& mapping,
    const std::vector<ObsSourceEntry>& sources
)
{
    combo->clear();

    if (isSceneAction(mapping.actionType))
    {
        combo->addItem(previousSceneTargetName, previousSceneTargetName);
        combo->addItem(nextSceneTargetName, nextSceneTargetName);

        auto scenes = getObsScenes();
        for (const auto& scene : scenes)
            combo->addItem(toQString(scene.name), toQString(scene.uuid));
    }
    else if (isTransitionAction(mapping.actionType))
    {
        combo->addItem(transitionTbarTargetName, transitionTbarTargetName);
        combo->addItem(transitionTriggerTargetName, transitionTriggerTargetName);
    }
    else
    {
        for (const auto& source : sources)
            combo->addItem(toQString(source.name), toQString(source.uuid));
    }

    int selectedIndex = -1;

    for (int i = 0; i < combo->count(); ++i)
    {
        auto itemText = combo->itemText(i);
        auto itemData = combo->itemData(i).toString();

        if (isSceneAction(mapping.actionType))
        {
            if ((mapping.targetName == previousSceneTargetName && itemText == previousSceneTargetName)
                || (mapping.targetName == nextSceneTargetName && itemText == nextSceneTargetName)
                || itemData == toQString(mapping.targetUuid))
            {
                selectedIndex = i;
                break;
            }
        }
        else if (isTransitionAction(mapping.actionType))
        {
            if ((mapping.targetName == transitionTbarTargetName && itemText == transitionTbarTargetName)
                || (mapping.targetName == transitionTriggerTargetName && itemText == transitionTriggerTargetName))
            {
                selectedIndex = i;
                break;
            }
        }
        else if (itemData == toQString(mapping.targetUuid))
        {
            selectedIndex = i;
            break;
        }
    }

    if (selectedIndex < 0 && combo->count() > 0)
        selectedIndex = 0;

    if (selectedIndex >= 0)
        combo->setCurrentIndex(selectedIndex);

    syncMappingTargetFromCombo(mapping, combo);
}

void layoutMappingsTableColumns(QTableWidget* table)
{
    if (table == nullptr)
        return;

    constexpr std::array<int, midi_to_obs_column_count> widths = {
        44,
        72,
        170,
        86,
        64,
        68,
        96,
        136,
        260,
        70,
        84,
        84,
    };

    for (int i = 0; i < midi_to_obs_column_count; ++i)
        table->setColumnWidth(i, widths[size_t(i)]);
}
} // namespace

namespace atk
{

MidiToObsDialog::MidiToObsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("atkAudio MIDI to OBS");
    resize(1200, 700);

    buildLayout();

    learnPollTimer = new QTimer(this);
    connect(learnPollTimer, &QTimer::timeout, this, [this]() { pollLearnState(); });
    learnPollTimer->start(100);

    reloadFromController();
}

MidiToObsDialog::~MidiToObsDialog() = default;

void MidiToObsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    reloadFromController();
}

void MidiToObsDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    layoutMappingsTableColumns(mappingsTable);
}

void MidiToObsDialog::buildLayout()
{
    mainLayout = new QVBoxLayout(this);

    auto* heading = new QLabel("MIDI to OBS mapping", this);
    heading->setStyleSheet("font-weight: bold; font-size: 14px;");
    mainLayout->addWidget(heading);

    statusLabel = new QLabel("Select MIDI inputs and create mappings.", this);
    mainLayout->addWidget(statusLabel);

    auto* deviceHeading = new QLabel("Subscribed MIDI inputs", this);
    deviceHeading->setStyleSheet("font-weight: bold;");
    mainLayout->addWidget(deviceHeading);

    deviceScrollArea = new QScrollArea(this);
    deviceScrollArea->setWidgetResizable(true);
    deviceContainer = new QWidget(deviceScrollArea);
    deviceLayout = new QVBoxLayout(deviceContainer);
    deviceLayout->setContentsMargins(4, 4, 4, 4);
    deviceLayout->setSpacing(4);
    deviceContainer->setLayout(deviceLayout);
    deviceScrollArea->setWidget(deviceContainer);
    deviceScrollArea->setMinimumHeight(120);
    mainLayout->addWidget(deviceScrollArea);

    auto* mappingHeading = new QLabel("Mappings", this);
    mappingHeading->setStyleSheet("font-weight: bold;");
    mainLayout->addWidget(mappingHeading);

    mappingsTable = new QTableWidget(this);
    mappingsTable->setColumnCount(midi_to_obs_column_count);
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_enabled, new QTableWidgetItem("On"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_learn, new QTableWidgetItem("Learn"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_device, new QTableWidgetItem("Device"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_type, new QTableWidgetItem("Type"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_channel, new QTableWidgetItem("Ch"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_data1, new QTableWidgetItem("Data"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_mode, new QTableWidgetItem("Mode"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_action, new QTableWidgetItem("Action"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_target, new QTableWidgetItem("Target"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_invert, new QTableWidgetItem("Invert"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_min, new QTableWidgetItem("Min"));
    mappingsTable->setHorizontalHeaderItem(midi_to_obs_column_max, new QTableWidgetItem("Max"));
    mappingsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mappingsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    mappingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mappingsTable->setMouseTracking(false);
    mappingsTable->viewport()->setMouseTracking(false);
    mappingsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    mappingsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mappingsTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    mainLayout->addWidget(mappingsTable, 1);
    layoutMappingsTableColumns(mappingsTable);

    auto* buttonRow = new QHBoxLayout();
    addButton = new QPushButton("Add Mapping", this);
    removeButton = new QPushButton("Remove Selected", this);
    refreshTargetsButton = new QPushButton("Refresh Targets", this);
    closeButton = new QPushButton("Close", this);

    buttonRow->addWidget(addButton);
    buttonRow->addWidget(removeButton);
    buttonRow->addWidget(refreshTargetsButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton);
    mainLayout->addLayout(buttonRow);

    connect(addButton, &QPushButton::clicked, this, [this]() { addMapping(); });
    connect(removeButton, &QPushButton::clicked, this, [this]() { removeSelectedMappings(); });
    connect(refreshTargetsButton, &QPushButton::clicked, this, [this]() { refreshSourceOptions(); });
    connect(closeButton, &QPushButton::clicked, this, [this]() { close(); });
}

void MidiToObsDialog::reloadFromController()
{
    if (auto* controller = MidiObsController::getInstance())
        dialogMappings = controller->getMappings();
    else
        dialogMappings.clear();

    rebuildSubscriptionWidgets();
    rebuildMappingsTable();
}

void MidiToObsDialog::rebuildSubscriptionWidgets()
{
    while (auto* item = deviceLayout->takeAt(0))
    {
        if (item->widget() != nullptr)
            item->widget()->deleteLater();

        delete item;
    }

    auto* controller = MidiObsController::getInstance();
    if (controller == nullptr)
        return;

    auto subscriptions = controller->getSubscriptions();
    auto devices = controller->getAvailableInputDevices();

    for (const auto& deviceName : devices)
    {
        auto* checkBox = new QCheckBox(toQString(deviceName), deviceContainer);
        checkBox->setChecked(subscriptions.subscribedInputDevices.contains(deviceName));
        connect(checkBox, &QCheckBox::toggled, this, [this](bool) { syncSubscriptionsToController(); });
        deviceLayout->addWidget(checkBox);
    }

    deviceLayout->addStretch(1);
}

bool MidiToObsDialog::isValidRow(int row) const
{
    return row >= 0 && row < int(dialogMappings.size());
}

void MidiToObsDialog::rebuildMappingsTable()
{
    int selectedRow = -1;
    if (auto* selectionModel = mappingsTable->selectionModel())
    {
        auto rows = selectionModel->selectedRows();
        if (!rows.isEmpty())
            selectedRow = rows.first().row();
    }

    rebuildingUi = true;

    QSignalBlocker tableBlocker(mappingsTable);

    auto* controller = MidiObsController::getInstance();
    auto devices = controller != nullptr ? controller->getAvailableInputDevices() : juce::StringArray();
    auto sources = getObsSources();

    mappingsTable->clearSelection();
    mappingsTable->clearContents();
    mappingsTable->setRowCount(int(dialogMappings.size()));

    for (int row = 0; row < int(dialogMappings.size()); ++row)
    {
        auto& mapping = dialogMappings[size_t(row)];

        auto* enabledCheck = new QCheckBox(mappingsTable);
        enabledCheck->setChecked(mapping.enabled);
        connect(
            enabledCheck,
            &QCheckBox::toggled,
            this,
            [this, row](bool enabled)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].enabled = enabled;
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_enabled, enabledCheck);

        auto* learnButton = new QPushButton("Learn", mappingsTable);
        connect(learnButton, &QPushButton::clicked, this, [this, row]() { startLearningForRow(row); });
        mappingsTable->setCellWidget(row, midi_to_obs_column_learn, learnButton);

        auto* deviceCombo = new QComboBox(mappingsTable);
        deviceCombo->addItem("Any", "");
        for (const auto& device : devices)
            deviceCombo->addItem(toQString(device), toQString(device));
        if (mapping.inputDeviceName.isNotEmpty())
            deviceCombo->setCurrentText(toQString(mapping.inputDeviceName));
        connect(
            deviceCombo,
            &QComboBox::currentTextChanged,
            this,
            [this, row](const QString& text)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].inputDeviceName = text == "Any" ? juce::String() : toJuceString(text);
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_device, deviceCombo);

        auto* typeCombo = new QComboBox(mappingsTable);
        addMessageTypeOptions(typeCombo);
        typeCombo->setCurrentIndex(typeCombo->findData(mapping.messageType));
        connect(
            typeCombo,
            &QComboBox::currentIndexChanged,
            this,
            [this, typeCombo, row](int)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].messageType = typeCombo->currentData().toInt();
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_type, typeCombo);

        auto* channelSpin = new QSpinBox(mappingsTable);
        channelSpin->setRange(0, 16);
        channelSpin->setSpecialValueText("Any");
        channelSpin->setValue(mapping.channel);
        connect(
            channelSpin,
            &QSpinBox::valueChanged,
            this,
            [this, row](int value)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].channel = value;
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_channel, channelSpin);

        auto* dataSpin = new QSpinBox(mappingsTable);
        dataSpin->setRange(0, 127);
        dataSpin->setValue(mapping.data1);
        connect(
            dataSpin,
            &QSpinBox::valueChanged,
            this,
            [this, row](int value)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].data1 = value;
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_data1, dataSpin);

        auto* modeCombo = new QComboBox(mappingsTable);
        addModeOptions(modeCombo);
        modeCombo->setCurrentIndex(modeCombo->findData(mapping.interactionMode));
        connect(
            modeCombo,
            &QComboBox::currentIndexChanged,
            this,
            [this, modeCombo, row](int)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].interactionMode = modeCombo->currentData().toInt();
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_mode, modeCombo);

        auto* actionCombo = new QComboBox(mappingsTable);
        addActionOptions(actionCombo);
        actionCombo->setCurrentIndex(actionCombo->findData(mapping.actionType));
        mappingsTable->setCellWidget(row, midi_to_obs_column_action, actionCombo);

        auto* targetCombo = new QComboBox(mappingsTable);
        populateTargetComboForMapping(targetCombo, mapping, sources);
        connect(
            targetCombo,
            &QComboBox::currentIndexChanged,
            this,
            [this, targetCombo, row](int)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                syncMappingTargetFromCombo(dialogMappings[size_t(row)], targetCombo);
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_target, targetCombo);

        connect(
            actionCombo,
            &QComboBox::currentIndexChanged,
            this,
            [this, actionCombo, row](int)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].actionType = actionCombo->currentData().toInt();
                dialogMappings[size_t(row)].targetUuid.clear();
                dialogMappings[size_t(row)].targetName.clear();

                applyMappingsToController();
                rebuildMappingsTable();

                if (row >= 0 && row < mappingsTable->rowCount())
                    mappingsTable->selectRow(row);
            }
        );

        auto* invertCheck = new QCheckBox(mappingsTable);
        invertCheck->setChecked(mapping.valueInverted);
        connect(
            invertCheck,
            &QCheckBox::toggled,
            this,
            [this, row](bool inverted)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].valueInverted = inverted;
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_invert, invertCheck);

        auto* minSpin = new QDoubleSpinBox(mappingsTable);
        minSpin->setRange(0.0, 1.0);
        minSpin->setDecimals(3);
        minSpin->setSingleStep(0.05);
        minSpin->setValue(mapping.minValue);
        connect(
            minSpin,
            &QDoubleSpinBox::valueChanged,
            this,
            [this, row](double value)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].minValue = value;
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_min, minSpin);

        auto* maxSpin = new QDoubleSpinBox(mappingsTable);
        maxSpin->setRange(0.0, 1.0);
        maxSpin->setDecimals(3);
        maxSpin->setSingleStep(0.05);
        maxSpin->setValue(mapping.maxValue);
        connect(
            maxSpin,
            &QDoubleSpinBox::valueChanged,
            this,
            [this, row](double value)
            {
                if (rebuildingUi)
                    return;

                if (!isValidRow(row))
                    return;

                dialogMappings[size_t(row)].maxValue = value;
                applyMappingsToController();
            }
        );
        mappingsTable->setCellWidget(row, midi_to_obs_column_max, maxSpin);
    }

    rebuildingUi = false;

    if (selectedRow >= 0 && selectedRow < mappingsTable->rowCount())
        mappingsTable->selectRow(selectedRow);

    layoutMappingsTableColumns(mappingsTable);
}

void MidiToObsDialog::syncSubscriptionsToController()
{
    if (rebuildingUi)
        return;

    MidiClientState subscriptions;

    for (int i = 0; i < deviceLayout->count(); ++i)
    {
        auto* item = deviceLayout->itemAt(i);
        auto* checkBox = item != nullptr ? qobject_cast<QCheckBox*>(item->widget()) : nullptr;

        if (checkBox != nullptr && checkBox->isChecked())
            subscriptions.subscribedInputDevices.add(toJuceString(checkBox->text()));
    }

    if (auto* controller = MidiObsController::getInstance())
        controller->setSubscriptions(subscriptions);

    rebuildMappingsTable();
}

void MidiToObsDialog::applyMappingsToController()
{
    if (rebuildingUi)
        return;

    if (auto* controller = MidiObsController::getInstance())
        controller->setMappings(dialogMappings);
}

void MidiToObsDialog::startLearningForRow(int row)
{
    if (!isValidRow(row))
        return;

    auto* controller = MidiObsController::getInstance();
    if (controller == nullptr)
        return;

    controller->beginLearning(dialogMappings[size_t(row)].mappingId);
    statusLabel->setText("Learning MIDI input for selected mapping...");
}

void MidiToObsDialog::removeSelectedMappings()
{
    auto* selectionModel = mappingsTable->selectionModel();
    if (selectionModel == nullptr)
        return;

    auto selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty())
        return;

    int row = selectedRows.first().row();

    if (!isValidRow(row))
        return;

    bool timerWasRunning = learnPollTimer != nullptr && learnPollTimer->isActive();
    if (timerWasRunning)
        learnPollTimer->stop();

    dialogMappings.erase(dialogMappings.begin() + row);
    applyMappingsToController();
    rebuildMappingsTable();

    if (timerWasRunning)
        learnPollTimer->start(100);
}

void MidiToObsDialog::addMapping()
{
    MidiObsMapping mapping;
    mapping.mappingId = createMidiObsMappingId();

    auto sources = getObsSources();
    if (!sources.empty())
    {
        mapping.targetUuid = sources.front().uuid;
        mapping.targetName = sources.front().name;
    }

    dialogMappings.push_back(mapping);
    applyMappingsToController();
    rebuildMappingsTable();

    int newRow = int(dialogMappings.size()) - 1;
    if (newRow >= 0 && newRow < mappingsTable->rowCount())
        mappingsTable->selectRow(newRow);
}

void MidiToObsDialog::refreshSourceOptions()
{
    rebuildMappingsTable();
}

void MidiToObsDialog::pollLearnState()
{
    auto* controller = MidiObsController::getInstance();
    if (controller == nullptr)
        return;

    auto learnState = controller->getLearnState();
    if (learnState.sequence == lastLearnSequence)
        return;

    lastLearnSequence = learnState.sequence;

    for (auto& mapping : dialogMappings)
    {
        if (mapping.mappingId != learnState.mappingId)
            continue;

        mapping.inputDeviceName = learnState.event.inputDeviceName;

        if (learnState.event.message.isController())
        {
            mapping.messageType = midi_obs_message_type_cc;
            mapping.data1 = learnState.event.message.getControllerNumber();
        }
        else if (learnState.event.message.isNoteOn())
        {
            mapping.messageType = midi_obs_message_type_note;
            mapping.data1 = learnState.event.message.getNoteNumber();
        }
        else if (learnState.event.message.isProgramChange())
        {
            mapping.messageType = midi_obs_message_type_program_change;
            mapping.data1 = learnState.event.message.getProgramChangeNumber();
        }

        mapping.channel = learnState.event.message.getChannel();
        break;
    }

    controller->setMappings(dialogMappings);
    statusLabel->setText(QString::fromUtf8("Learned from ") + toQString(learnState.event.inputDeviceName));
    rebuildMappingsTable();
}

} // namespace atk

#endif
