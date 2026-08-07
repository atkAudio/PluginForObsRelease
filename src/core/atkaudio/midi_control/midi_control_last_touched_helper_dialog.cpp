#include "midi_control_last_touched_helper_dialog.h"

#include <atkaudio/GlobalSettings.h>
#include <atkaudio/midi_control/midi_control_controller.h>
#include <atkaudio/midi_control/midi_control_mapping.h>
#include <atkaudio/midi_control/last_touched_parameter_tracker.h>
#include <atkaudio/Logging.h>
#include <atkaudio/midi_control/obs_filter_last_touched_tracker.h>
#include <atkaudio/midi_control/midi_control_dialog.h>

#include <obs-frontend-api.h>

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <algorithm>
#include <string_view>
#include <unordered_set>

namespace
{
enum LastTouchedSection
{
    last_touched_section_plugin = 0,
    last_touched_section_audio,
    last_touched_section_video,
};

enum LastTouchedHistoryColumn
{
    last_touched_history_column_offset = 0,
    last_touched_history_column_parameter,
    last_touched_history_column_value,
    last_touched_history_column_owner,
    last_touched_history_column_count,
};

constexpr auto lastTouchedDockId = "atkaudio_midiobs_last_touched_dock";
constexpr auto lastTouchedDockTitle = "MIDI Control Last Touched";
constexpr int mappingContextMin = atk::midi_control_mapping_context_min;
constexpr int mappingContextMax = atk::midi_control_mapping_context_max;
constexpr std::string_view effectParmObsFilterLayerPrefix = "effect_parm_obs_filter:";
constexpr std::string_view effectParmIndexPrefix = "effect_parm_index:";

atk::MidiControlLastTouchedHelperDialog*& getLastTouchedDockInstance()
{
    static atk::MidiControlLastTouchedHelperDialog* instance = nullptr;
    return instance;
}

QString toQString(const juce::String& text)
{
    return QString::fromUtf8(text.toRawUTF8());
}

juce::String toJuceString(const QString& text)
{
    return juce::String::fromUTF8(text.toUtf8().constData());
}

juce::String createEffectParmIndexToken(int parameterIndex)
{
    if (parameterIndex < 0)
        return {};

    return juce::String(effectParmIndexPrefix.data()) + juce::String(parameterIndex);
}

bool isMappingInContext(const atk::MidiControlMapping& mapping, int bank, int preset)
{
    bool bankMatches = mapping.bank == atk::midi_control_mapping_context_any || mapping.bank == bank;
    bool presetMatches = mapping.preset == atk::midi_control_mapping_context_any || mapping.preset == preset;
    return bankMatches && presetMatches;
}

bool hasSameInputSignature(const atk::MidiControlMapping& left, const atk::MidiControlMapping& right)
{
    return left.inputDeviceName == right.inputDeviceName
        && left.messageType == right.messageType
        && left.channel == right.channel
        && left.data1 == right.data1;
}

juce::String getSourceNameByUuid(const juce::String& sourceUuid)
{
    if (sourceUuid.isEmpty())
        return {};

    auto* source = obs_get_source_by_uuid(sourceUuid.toRawUTF8());
    if (source == nullptr)
        return {};

    juce::String sourceName = juce::String(obs_source_get_name(source));
    obs_source_release(source);

    return sourceName;
}

void raiseDockWindow(atk::MidiControlLastTouchedHelperDialog* dockWidget)
{
    if (dockWidget == nullptr)
        return;

    if (QWidget* parentDock = dockWidget->parentWidget())
    {
        parentDock->show();
        parentDock->raise();
        parentDock->activateWindow();
        return;
    }

    dockWidget->show();
    dockWidget->raise();
    dockWidget->activateWindow();
}

juce::String getFilterNameByUuid(const juce::String& filterUuid)
{
    if (filterUuid.isEmpty())
        return {};

    auto* filter = obs_get_source_by_uuid(filterUuid.toRawUTF8());
    if (filter == nullptr)
        return {};

    juce::String filterName = juce::String(obs_source_get_name(filter));
    obs_source_release(filter);

    return filterName;
}

juce::String findFilterUuidByNameOnSource(const juce::String& sourceUuid, const juce::String& filterName)
{
    if (sourceUuid.isEmpty() || filterName.isEmpty())
        return {};

    auto* source = obs_get_source_by_uuid(sourceUuid.toRawUTF8());
    if (source == nullptr)
        return {};

    auto* filter = obs_source_get_filter_by_name(source, filterName.toRawUTF8());
    juce::String matchedFilterUuid;

    if (filter != nullptr)
    {
        auto* currentUuid = obs_source_get_uuid(filter);
        if (currentUuid != nullptr)
            matchedFilterUuid = juce::String(currentUuid);

        obs_source_release(filter);
    }

    obs_source_release(source);
    return matchedFilterUuid;
}

juce::String getOwningFilterDisplayName(const juce::String& ownerFilterName, const juce::AudioProcessor* processor)
{
    auto obsFilterName = ownerFilterName.isNotEmpty() ? ownerFilterName : juce::String("Unknown Filter");
    auto hostedPluginName = processor != nullptr ? processor->getName() : juce::String();

    if (hostedPluginName.isEmpty() || obsFilterName == hostedPluginName)
        return obsFilterName;

    return obsFilterName + " -> " + hostedPluginName;
}

} // namespace

namespace atk
{

struct MidiControlLastTouchedHelperDialog::SaveEffectParmTarget
{
    juce::String sourceUuid;
    juce::String targetName;
    juce::String targetLayerToken;
    juce::String targetLayerName;
    juce::String targetParameterToken;
    juce::String targetParameterName;
};

MidiControlLastTouchedHelperDialog::MidiControlLastTouchedHelperDialog(QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(lastTouchedDockTitle);
    resize(620, 280);

    buildLayout();
    loadSaveDefaults();

    refreshNow();
}

MidiControlLastTouchedHelperDialog::~MidiControlLastTouchedHelperDialog() = default;

void MidiControlLastTouchedHelperDialog::buildLayout()
{
    auto* layout = new QVBoxLayout(this);

    auto* saveRow = new QHBoxLayout();
    auto* scopeLabel = new QLabel("Scope", this);
    saveScopeCombo = new QComboBox(this);
    saveScopeCombo->addItem("Plugin", save_scope_plugin);
    saveScopeCombo->addItem("Audio", save_scope_audio);
    saveScopeCombo->addItem("Video", save_scope_video);

    auto* bankLabel = new QLabel("Bank", this);
    destinationBankSpin = new QSpinBox(this);
    destinationBankSpin->setRange(mappingContextMin, mappingContextMax);

    auto* presetLabel = new QLabel("Preset", this);
    destinationPresetSpin = new QSpinBox(this);
    destinationPresetSpin->setRange(mappingContextMin, mappingContextMax);

    saveAsPresetButton = new QPushButton("Save As Preset", this);

    saveRow->addWidget(scopeLabel);
    saveRow->addWidget(saveScopeCombo);
    saveRow->addWidget(bankLabel);
    saveRow->addWidget(destinationBankSpin);
    saveRow->addWidget(presetLabel);
    saveRow->addWidget(destinationPresetSpin);
    saveRow->addWidget(saveAsPresetButton);
    saveRow->addStretch(1);
    layout->addLayout(saveRow);

    auto* slotRow = new QHBoxLayout();
    auto* slotLabel = new QLabel("Slots", this);
    slotRow->addWidget(slotLabel);

    for (int i = 0; i < int(slotChecks.size()); ++i)
    {
        auto* check = new QCheckBox(QString::number(i + 1), this);
        slotChecks[size_t(i)] = check;
        slotRow->addWidget(check);
        connect(
            check,
            &QCheckBox::toggled,
            this,
            [this](bool)
            {
                updateSaveButtonState();
                saveCurrentDefaults();
            }
        );
    }

    slotRow->addStretch(1);
    layout->addLayout(slotRow);

    saveStatusLabel = new QLabel("", this);
    layout->addWidget(saveStatusLabel);

    historyTable = new QTableView(this);
    historyModel = new QStandardItemModel(this);
    historyModel->setColumnCount(last_touched_history_column_count);
    historyModel->setHeaderData(last_touched_history_column_offset, Qt::Horizontal, "#");
    historyModel->setHeaderData(last_touched_history_column_parameter, Qt::Horizontal, "Param");
    historyModel->setHeaderData(last_touched_history_column_value, Qt::Horizontal, "Val");
    historyModel->setHeaderData(last_touched_history_column_owner, Qt::Horizontal, "Owner");
    historyTable->setModel(historyModel);
    historyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    historyTable->setSelectionMode(QAbstractItemView::SingleSelection);
    historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyTable->verticalHeader()->setVisible(false);
    historyTable->setShowGrid(false);
    historyTable->setWordWrap(false);
    historyTable->setTextElideMode(Qt::ElideRight);
    historyTable->setAlternatingRowColors(true);
    historyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    historyTable->horizontalHeader()->setSectionResizeMode(last_touched_history_column_parameter, QHeaderView::Stretch);
    layout->addWidget(historyTable, 1);

    historyTable->setColumnWidth(last_touched_history_column_offset, 32);
    historyTable->setColumnWidth(last_touched_history_column_value, 72);
    historyTable->setColumnWidth(last_touched_history_column_owner, 180);
    historyTable->verticalHeader()->setDefaultSectionSize(20);

    auto* buttonRow = new QHBoxLayout();
    holdHistoryButton = new QPushButton("Hold", this);
    holdHistoryButton->setCheckable(true);
    clearHistoryButton = new QPushButton("Clear History", this);
    refreshButton = new QPushButton("Refresh", this);

    buttonRow->addWidget(holdHistoryButton);
    buttonRow->addWidget(clearHistoryButton);
    buttonRow->addWidget(refreshButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    connect(
        holdHistoryButton,
        &QPushButton::toggled,
        this,
        [this](bool checked)
        {
            LastTouchedParameterTracker::getInstance().setHistoryHoldEnabled(checked);

            if (auto* controller = MidiControlController::getInstanceWithoutCreating())
                controller->requestFeedbackRefresh();

            refreshNow();
        }
    );
    connect(
        clearHistoryButton,
        &QPushButton::clicked,
        this,
        [this]()
        {
            LastTouchedParameterTracker::getInstance().clear();
            ObsFilterLastTouchedTracker::getInstance().clear();

            if (auto* controller = MidiControlController::getInstanceWithoutCreating())
                controller->requestFeedbackRefresh();

            if (saveStatusLabel != nullptr)
                saveStatusLabel->setText("Cleared last touched history");

            refreshNow();
        }
    );
    connect(refreshButton, &QPushButton::clicked, this, [this]() { refreshNow(); });
    connect(
        saveScopeCombo,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        this,
        [this](int) { saveCurrentDefaults(); }
    );
    connect(destinationBankSpin, &QSpinBox::valueChanged, this, [this](int) { saveCurrentDefaults(); });
    connect(destinationPresetSpin, &QSpinBox::valueChanged, this, [this](int) { saveCurrentDefaults(); });
    connect(saveAsPresetButton, &QPushButton::clicked, this, [this]() { onSaveAsPresetClicked(); });

    updateSaveButtonState();
}

void MidiControlLastTouchedHelperDialog::updateSaveButtonState()
{
    if (saveAsPresetButton == nullptr)
        return;

    saveAsPresetButton->setEnabled(!getSelectedSlots().empty());
}

std::vector<int> MidiControlLastTouchedHelperDialog::getSelectedSlots() const
{
    std::vector<int> selectedSlotList;

    for (int i = 0; i < int(slotChecks.size()); ++i)
    {
        auto* check = slotChecks[size_t(i)];
        if (check == nullptr || !check->isChecked())
            continue;

        selectedSlotList.push_back(i + 1);
    }

    return selectedSlotList;
}

void MidiControlLastTouchedHelperDialog::loadSaveDefaults()
{
    if (saveScopeCombo == nullptr || destinationBankSpin == nullptr || destinationPresetSpin == nullptr)
        return;

    QSignalBlocker scopeBlocker(saveScopeCombo);
    QSignalBlocker bankBlocker(destinationBankSpin);
    QSignalBlocker presetBlocker(destinationPresetSpin);

    auto scope = settings::getMidiControlLastTouchedPresetScope();
    auto scopeIndex = saveScopeCombo->findData(scope);
    if (scopeIndex < 0)
        scopeIndex = saveScopeCombo->findData(save_scope_plugin);
    if (scopeIndex < 0)
        scopeIndex = 0;
    saveScopeCombo->setCurrentIndex(scopeIndex);

    auto storedSlots = settings::getMidiControlLastTouchedPresetSlots();
    if (storedSlots.empty())
        storedSlots = {1, 2, 3, 4, 5, 6, 7, 8};

    for (int i = 0; i < int(slotChecks.size()); ++i)
    {
        auto* check = slotChecks[size_t(i)];
        if (check == nullptr)
            continue;

        QSignalBlocker slotBlocker(check);
        auto slot = i + 1;
        auto selected = std::find(storedSlots.begin(), storedSlots.end(), slot) != storedSlots.end();
        check->setChecked(selected);
    }

    auto lastBank = settings::getMidiControlLastTouchedPresetBank();
    auto lastPreset = settings::getMidiControlLastTouchedPresetPreset();

    auto nextBank = mappingContextMin;
    auto nextPreset = mappingContextMin;

    if (lastBank >= mappingContextMin
        && lastBank <= mappingContextMax
        && lastPreset >= mappingContextMin
        && lastPreset <= mappingContextMax)
    {
        nextBank = lastBank;
        nextPreset = lastPreset + 1;
        if (nextPreset > mappingContextMax)
        {
            nextPreset = mappingContextMin;
            nextBank += 1;
            if (nextBank > mappingContextMax)
                nextBank = mappingContextMin;
        }
    }

    destinationBankSpin->setValue(nextBank);
    destinationPresetSpin->setValue(nextPreset);

    updateSaveButtonState();
}

void MidiControlLastTouchedHelperDialog::saveCurrentDefaults()
{
    if (saveScopeCombo == nullptr || destinationBankSpin == nullptr || destinationPresetSpin == nullptr)
        return;

    settings::setMidiControlLastTouchedPresetScope(saveScopeCombo->currentData().toInt());
    settings::setMidiControlLastTouchedPresetSlots(getSelectedSlots());
}

bool MidiControlLastTouchedHelperDialog::resolveEffectParmTargetForSelection(
    int scope,
    int slotIndex,
    SaveEffectParmTarget& target,
    juce::String& error
) const
{
    target = {};
    error.clear();

    if (slotIndex < 0 || slotIndex > 7)
    {
        error = "slot out of range";
        return false;
    }

    if (scope == save_scope_plugin)
    {
        LastTouchedParameterEntry entry;
        if (!LastTouchedParameterTracker::getInstance().getParameterAtOffset(slotIndex, entry))
        {
            error = "no plugin entry at selected slot";
            return false;
        }

        if (entry.ownerSourceUuid.isEmpty() || entry.ownerFilterName.isEmpty() || entry.parameterIndex < 0)
        {
            error = "plugin entry is missing owner/filter/index";
            return false;
        }

        auto filterUuid = findFilterUuidByNameOnSource(entry.ownerSourceUuid, entry.ownerFilterName);
        if (filterUuid.isEmpty())
        {
            error = "plugin filter could not be resolved on source";
            return false;
        }

        auto parameterToken = createEffectParmIndexToken(entry.parameterIndex);
        if (parameterToken.isEmpty())
        {
            error = "plugin parameter index token is invalid";
            return false;
        }

        target.sourceUuid = entry.ownerSourceUuid;
        target.targetName = getSourceNameByUuid(entry.ownerSourceUuid);
        if (target.targetName.isEmpty())
        {
            error = "owner source is unavailable";
            return false;
        }

        target.targetLayerToken = juce::String(effectParmObsFilterLayerPrefix.data()) + filterUuid;
        target.targetLayerName = entry.ownerFilterName;
        target.targetParameterToken = parameterToken;
        target.targetParameterName =
            entry.parameter != nullptr ? entry.parameter->getName(128).trim() : juce::String(parameterToken);
        return true;
    }

    ObsFilterLastTouchedEntry entry;
    bool found = false;

    if (scope == save_scope_audio)
        found = ObsFilterLastTouchedTracker::getInstance().getAudioParameterAtOffset(slotIndex, entry);
    else if (scope == save_scope_video)
        found = ObsFilterLastTouchedTracker::getInstance().getVideoParameterAtOffset(slotIndex, entry);

    if (!found)
    {
        error = "no OBS filter entry at selected slot";
        return false;
    }

    if (entry.sourceUuid.isEmpty() || entry.filterUuid.isEmpty() || entry.parameterKey.isEmpty())
    {
        error = "OBS filter entry is missing source/filter/parameter";
        return false;
    }

    target.sourceUuid = entry.sourceUuid;
    target.targetName = getSourceNameByUuid(entry.sourceUuid);
    if (target.targetName.isEmpty())
    {
        error = "owner source is unavailable";
        return false;
    }

    target.targetLayerToken = juce::String(effectParmObsFilterLayerPrefix.data()) + entry.filterUuid;
    target.targetLayerName = getFilterNameByUuid(entry.filterUuid);
    if (target.targetLayerName.isEmpty())
    {
        error = "filter is unavailable";
        return false;
    }

    target.targetParameterToken = entry.parameterKey;
    target.targetParameterName = entry.parameterKey;
    return true;
}

void MidiControlLastTouchedHelperDialog::onSaveAsPresetClicked()
{
    auto* controller = MidiControlController::getInstanceWithoutCreating();
    if (controller == nullptr)
    {
        if (saveStatusLabel != nullptr)
            saveStatusLabel->setText("Save failed: controller unavailable");
        return;
    }

    auto selectedSlots = getSelectedSlots();
    if (selectedSlots.empty())
    {
        if (saveStatusLabel != nullptr)
            saveStatusLabel->setText("Select at least one slot");
        updateSaveButtonState();
        return;
    }

    if (saveScopeCombo == nullptr || destinationBankSpin == nullptr || destinationPresetSpin == nullptr)
        return;

    auto scope = saveScopeCombo->currentData().toInt();
    auto destinationBank = destinationBankSpin->value();
    auto destinationPreset = destinationPresetSpin->value();
    auto activeBank = controller->getActiveBank();
    auto activePreset = controller->getActivePreset();

    auto mappings = controller->getMappings();
    std::vector<MidiControlMapping> generatedMappings;
    juce::StringArray skippedReasons;

    for (auto slot : selectedSlots)
    {
        auto slotOffset = slot - 1;

        SaveEffectParmTarget target;
        juce::String resolveError;
        if (!resolveEffectParmTargetForSelection(scope, slotOffset, target, resolveError))
        {
            skippedReasons.add("slot " + juce::String(slot) + ": " + resolveError);
            continue;
        }

        juce::String sourceTargetToken;
        if (scope == save_scope_plugin)
            sourceTargetToken = createLastTouchedTargetToken(slotOffset);
        else if (scope == save_scope_audio)
            sourceTargetToken = createObsFilterAudioLastTouchedTargetToken(slotOffset);
        else
            sourceTargetToken = createObsFilterVideoLastTouchedTargetToken(slotOffset);

        auto templateCountForSlot = 0;

        for (auto& mapping : mappings)
        {
            if (!mapping.enabled)
                continue;

            if (mapping.actionType != midi_control_action_type_last_touched)
                continue;

            if (mapping.targetUuid != sourceTargetToken)
                continue;

            if (!isMappingInContext(mapping, activeBank, activePreset))
                continue;

            MidiControlMapping generated = mapping;
            generated.mappingId = createMidiControlMappingId();
            generated.actionType = midi_control_action_type_effect_parm;
            generated.targetUuid = target.sourceUuid;
            generated.targetName = target.targetName;
            generated.targetLayerToken = target.targetLayerToken;
            generated.targetLayerName = target.targetLayerName;
            generated.targetParameterToken = target.targetParameterToken;
            generated.targetParameterName = target.targetParameterName;
            generated.bank = destinationBank;
            generated.preset = destinationPreset;
            generatedMappings.push_back(std::move(generated));
            templateCountForSlot += 1;
        }

        if (templateCountForSlot == 0)
            skippedReasons.add("slot " + juce::String(slot) + ": no active template mapping found");
    }

    if (generatedMappings.empty())
    {
        if (saveStatusLabel != nullptr)
        {
            auto status = juce::String("Saved 0 mappings");
            if (skippedReasons.size() > 0)
                status += " (" + juce::String(skippedReasons.size()) + " skipped)";

            saveStatusLabel->setText(toQString(status));
        }
        return;
    }

    int collisionCount = 0;
    for (auto& generated : generatedMappings)
    {
        for (auto& existing : mappings)
        {
            if (!existing.enabled)
                continue;

            if (existing.bank != destinationBank || existing.preset != destinationPreset)
                continue;

            if (hasSameInputSignature(existing, generated))
            {
                collisionCount += 1;
                break;
            }
        }
    }

    mappings.insert(mappings.end(), generatedMappings.begin(), generatedMappings.end());

    juce::String validationError;
    if (!validateMidiControlMappings(mappings, validationError))
    {
        if (saveStatusLabel != nullptr)
            saveStatusLabel->setText(toQString("Save failed: " + validationError));
        return;
    }

    controller->setMappings(mappings);
    controller->requestFeedbackRefresh();
    MidiControlDialog::refreshIfOpen();

    settings::setMidiControlLastTouchedPresetScope(scope);
    settings::setMidiControlLastTouchedPresetSlots(selectedSlots);
    settings::setMidiControlLastTouchedPresetBank(destinationBank);
    settings::setMidiControlLastTouchedPresetPreset(destinationPreset);

    auto status = juce::String("Saved ") + juce::String(int(generatedMappings.size())) + " mapping(s)";
    if (skippedReasons.size() > 0)
        status += ", skipped " + juce::String(skippedReasons.size());
    if (collisionCount > 0)
        status += ", warning: " + juce::String(collisionCount) + " destination signature collision(s)";

    if (saveStatusLabel != nullptr)
        saveStatusLabel->setText(toQString(status));
}

void MidiControlLastTouchedHelperDialog::refreshNow()
{
    struct TableRow
    {
        int section = last_touched_section_plugin;
        juce::String lane;
        juce::String parameterName;
        double normalizedValue = 0.0;
        juce::String formattedValue;
        juce::String ownerSourceUuid;
        juce::String owningFilterName;
        uint64_t touchSequence = 0;
    };

    std::vector<TableRow> pluginRows;
    std::vector<TableRow> audioRows;
    std::vector<TableRow> videoRows;

    auto pluginHistory = LastTouchedParameterTracker::getInstance().getRecentParametersSnapshot();
    pluginRows.reserve(pluginHistory.size());

    for (const auto& entry : pluginHistory)
    {
        auto* parameter = entry.parameter;
        if (parameter == nullptr)
            continue;

        auto parameterName = juce::String();
        double normalizedValue = 0.0;
        auto formattedValue = juce::String();

        parameterName = parameter->getName(100);
        normalizedValue = juce::jlimit(0.0, 1.0, double(parameter->getValue()));
        formattedValue = parameter->getCurrentValueAsText();

        if (parameterName.isEmpty())
            parameterName = "<unnamed parameter>";

        TableRow row;
        row.section = last_touched_section_plugin;
        row.lane = "Plugin";
        row.parameterName = parameterName;
        row.normalizedValue = normalizedValue;
        row.formattedValue = formattedValue;
        row.ownerSourceUuid = entry.ownerSourceUuid;
        row.owningFilterName = getOwningFilterDisplayName(entry.ownerFilterName, entry.processor);
        row.touchSequence = entry.touchSequence;
        pluginRows.push_back(std::move(row));
    }

    auto obsFilterHistory = ObsFilterLastTouchedTracker::getInstance().getRecentParametersSnapshot();
    audioRows.reserve(obsFilterHistory.size());
    videoRows.reserve(obsFilterHistory.size());

    for (const auto& entry : obsFilterHistory)
    {
        if (entry.parameterKey.isEmpty())
            continue;

        auto hasAudioLane = (entry.laneFlags & obs_filter_last_touched_lane_audio) != 0;
        auto hasVideoLane = (entry.laneFlags & obs_filter_last_touched_lane_video) != 0;
        if (!hasAudioLane && !hasVideoLane)
            continue;

        auto createObsRow = [&entry](juce::String laneName, int section)
        {
            TableRow row;
            row.section = section;
            row.lane = laneName;
            row.parameterName = entry.parameterKey;
            row.normalizedValue = juce::jlimit(0.0, 1.0, entry.normalizedValue);
            row.formattedValue = juce::String(row.normalizedValue, 4);
            row.ownerSourceUuid = entry.sourceUuid;
            row.owningFilterName = getFilterNameByUuid(entry.filterUuid);
            row.touchSequence = entry.touchSequence;
            return row;
        };

        if (hasAudioLane)
            audioRows.push_back(createObsRow("Audio", last_touched_section_audio));

        if (hasVideoLane)
            videoRows.push_back(createObsRow("Video", last_touched_section_video));
    }

    auto sortRowsByTouchSequence = [](std::vector<TableRow>& rows)
    {
        std::sort(
            rows.begin(),
            rows.end(),
            [](const TableRow& left, const TableRow& right) { return left.touchSequence > right.touchSequence; }
        );
    };

    sortRowsByTouchSequence(pluginRows);
    sortRowsByTouchSequence(audioRows);
    sortRowsByTouchSequence(videoRows);

    struct SectionRows
    {
        juce::String sectionTitle;
        std::vector<TableRow>* rows = nullptr;
    };

    std::array<SectionRows, 3> sections = {
        SectionRows{"Plugin Parameters", &pluginRows},
        SectionRows{ "Audio Parameters",  &audioRows},
        SectionRows{ "Video Parameters",  &videoRows},
    };

    auto totalRowCount = 0;
    for (auto& section : sections)
    {
        totalRowCount += 1;
        totalRowCount += section.rows->empty() ? 1 : int(section.rows->size());
    }

    historyTable->clearSpans();
    historyModel->setRowCount(totalRowCount);

    uint64_t newestTouchSequence = 0;

    auto tableRow = 0;
    for (auto& section : sections)
    {
        auto* sectionItem = new QStandardItem(toQString(section.sectionTitle));
        auto sectionFont = sectionItem->font();
        sectionFont.setBold(true);
        sectionItem->setFont(sectionFont);
        sectionItem->setEditable(false);
        sectionItem->setSelectable(false);
        historyTable->setSpan(tableRow, last_touched_history_column_offset, 1, last_touched_history_column_count);
        historyModel->setItem(tableRow, last_touched_history_column_offset, sectionItem);
        ++tableRow;

        if (section.rows->empty())
        {
            auto* emptyItem = new QStandardItem("No entries yet");
            auto emptyFont = emptyItem->font();
            emptyFont.setItalic(true);
            emptyItem->setFont(emptyFont);
            emptyItem->setEditable(false);
            emptyItem->setSelectable(false);
            historyTable->setSpan(tableRow, last_touched_history_column_offset, 1, last_touched_history_column_count);
            historyModel->setItem(tableRow, last_touched_history_column_offset, emptyItem);
            ++tableRow;
            continue;
        }

        auto sectionOffset = 0;
        for (auto& entry : *section.rows)
        {
            auto ownerSourceName = getSourceNameByUuid(entry.ownerSourceUuid);
            if (ownerSourceName.isEmpty())
                ownerSourceName = "<missing source>";

            auto* offsetItem = new QStandardItem(QString::number(sectionOffset + 1));
            auto* parameterItem = new QStandardItem(toQString(entry.parameterName));
            auto* valueItem = new QStandardItem(toQString(entry.formattedValue));
            auto ownerSummary = ownerSourceName;
            if (entry.owningFilterName.isNotEmpty())
                ownerSummary = ownerSummary + " / " + entry.owningFilterName;
            auto* ownerItem = new QStandardItem(toQString(ownerSummary));

            offsetItem->setEditable(false);
            parameterItem->setEditable(false);
            valueItem->setEditable(false);
            ownerItem->setEditable(false);

            historyModel->setItem(tableRow, last_touched_history_column_offset, offsetItem);
            historyModel->setItem(tableRow, last_touched_history_column_parameter, parameterItem);
            historyModel->setItem(tableRow, last_touched_history_column_value, valueItem);
            historyModel->setItem(tableRow, last_touched_history_column_owner, ownerItem);

            if (entry.touchSequence > newestTouchSequence)
                newestTouchSequence = entry.touchSequence;

            ++sectionOffset;
            ++tableRow;
        }
    }

    lastRenderedTouchSequence = newestTouchSequence;

    if (holdHistoryButton != nullptr)
    {
        bool holdEnabled = LastTouchedParameterTracker::getInstance().isHistoryHoldEnabled();
        QSignalBlocker blocker(holdHistoryButton);
        holdHistoryButton->setChecked(holdEnabled);
    }
}

void MidiControlLastTouchedHelperDialog::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateRestoreStateSetting();
    refreshNow();
}

void MidiControlLastTouchedHelperDialog::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    updateRestoreStateSetting();
}

void MidiControlLastTouchedHelperDialog::updateRestoreStateSetting() const
{
    settings::setRestoreMidiControlLastTouchedDock(isVisible() || parentWidget() != nullptr);
}

bool MidiControlLastTouchedHelperDialog::shouldRestoreOnStartup()
{
    return settings::shouldRestoreMidiControlLastTouchedDock();
}

void MidiControlLastTouchedHelperDialog::ensureRegistered()
{
    auto*& instance = getLastTouchedDockInstance();

    if (instance != nullptr)
        return;

    instance = new MidiControlLastTouchedHelperDialog();
    instance->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(instance, &QObject::destroyed, instance, [](QObject*) { getLastTouchedDockInstance() = nullptr; });

    obs_frontend_add_dock_by_id(lastTouchedDockId, lastTouchedDockTitle, instance);
}

void MidiControlLastTouchedHelperDialog::unregisterDock()
{
    auto*& instance = getLastTouchedDockInstance();
    if (instance == nullptr)
        return;

    auto* application = QCoreApplication::instance();
    if (application == nullptr)
        return;

    QMetaObject::invokeMethod(application, []() { obs_frontend_remove_dock(lastTouchedDockId); }, Qt::QueuedConnection);
}

void MidiControlLastTouchedHelperDialog::showOrCreateAndRaise()
{
    ensureRegistered();

    auto* instance = getLastTouchedDockInstance();
    if (instance == nullptr)
        return;

    instance->refreshNow();
    QTimer::singleShot(0, instance, [instance]() { raiseDockWindow(instance); });
}

void MidiControlLastTouchedHelperDialog::refreshIfOpen()
{
    auto* instance = getLastTouchedDockInstance();
    if (instance != nullptr && instance->isVisible())
        instance->refreshNow();
}

} // namespace atk
