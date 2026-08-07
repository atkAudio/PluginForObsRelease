#include "midi_control_mappings_table_model.h"

#include <algorithm>

namespace
{
enum MidiControlTableColumn
{
    midi_control_column_enabled = 0,
    midi_control_column_learn,
    midi_control_column_input_device,
    midi_control_column_output_route,
    midi_control_column_type,
    midi_control_column_channel,
    midi_control_column_data1,
    midi_control_column_mode,
    midi_control_column_action,
    midi_control_column_target,
    midi_control_column_layer,
    midi_control_column_parameter,
    midi_control_column_bank,
    midi_control_column_preset,
    midi_control_column_activation,
    midi_control_column_invert,
    midi_control_column_min,
    midi_control_column_max,
    midi_control_column_scale,
    midi_control_column_count,
};
} // namespace

namespace atk
{

namespace
{
QString toQString(juce::String text)
{
    return QString::fromUtf8(text.toRawUTF8());
}

juce::String toJuceString(QString text)
{
    return juce::String::fromUTF8(text.toUtf8().constData());
}

QString describeOutputRoute(const MidiControlMapping& mapping)
{
    if (mapping.outputMode == midi_control_output_mode_none)
        return "None";

    if (mapping.outputMode == midi_control_output_mode_any)
        return "Any";

    if (mapping.outputDeviceName.isEmpty())
        return "None";

    return toQString(mapping.outputDeviceName);
}

QString describeMessageType(int messageType)
{
    switch (messageType)
    {
    case midi_control_message_type_cc:
        return "CC";
    case midi_control_message_type_note:
        return "Note";
    case midi_control_message_type_program_change:
        return "Program";
    default:
        return "Unknown";
    }
}

QString describeInteractionMode(int interactionMode)
{
    switch (interactionMode)
    {
    case midi_control_interaction_mode_absolute:
        return "Absolute";
    case midi_control_interaction_mode_trigger:
        return "Trigger";
    case midi_control_interaction_mode_toggle:
        return "Toggle";
    case midi_control_interaction_mode_inc_dec:
        return "Inc/Dec";
    case midi_control_interaction_mode_inc_dec2:
        return "Inc/Dec2";
    default:
        return "Unknown";
    }
}

QString describeActionType(int actionType)
{
    auto options = getMidiControlActionOptions();
    for (auto& option : options)
        if (option.actionType == actionType)
            return toQString(option.displayName);

    return "Unknown";
}

QString describeActivationMode(int activationMode)
{
    switch (activationMode)
    {
    case midi_control_activation_mode_scene:
        return "Scene";
    case midi_control_activation_mode_global:
        return "Global";
    case midi_control_activation_mode_preview:
        return "Preview";
    default:
        return "Unknown";
    }
}
} // namespace

int MidiControlMappingsTableModel::getColumnCount()
{
    return midi_control_column_count;
}

MidiControlMappingsTableModel::MidiControlMappingsTableModel(
    std::vector<MidiControlMapping>* mappingsIn,
    QObject* parent
)
    : QAbstractTableModel(parent)
    , mappings(mappingsIn)
{
}

int MidiControlMappingsTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() || mappings == nullptr)
        return 0;

    return int(mappings->size());
}

int MidiControlMappingsTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;

    return midi_control_column_count;
}

QVariant MidiControlMappingsTableModel::data(const QModelIndex& index, int role) const
{
    if (mappings == nullptr || !index.isValid())
        return {};

    auto row = index.row();
    if (row < 0 || row >= int(mappings->size()))
        return {};

    auto& mapping = (*mappings)[size_t(row)];
    auto column = index.column();

    if (role == Qt::CheckStateRole)
    {
        if (column == midi_control_column_enabled)
            return mapping.enabled ? Qt::Checked : Qt::Unchecked;

        if (column == midi_control_column_invert)
            return mapping.valueInverted ? Qt::Checked : Qt::Unchecked;
    }

    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return {};

    if (role == Qt::DisplayRole)
    {
        switch (column)
        {
        case midi_control_column_enabled:
        case midi_control_column_invert:
            return {};
        case midi_control_column_type:
            return describeMessageType(mapping.messageType);
        case midi_control_column_mode:
            return describeInteractionMode(mapping.interactionMode);
        case midi_control_column_action:
            return describeActionType(mapping.actionType);
        case midi_control_column_activation:
            return describeActivationMode(mapping.activationMode);
        default:
            break;
        }
    }

    switch (column)
    {
    case midi_control_column_enabled:
        return {};
    case midi_control_column_learn:
        return isLearningRow(row) ? "Learning" : "Learn";
    case midi_control_column_input_device:
        return mapping.inputDeviceName.isEmpty() ? "Any" : toQString(mapping.inputDeviceName);
    case midi_control_column_output_route:
        return describeOutputRoute(mapping);
    case midi_control_column_type:
        return mapping.messageType;
    case midi_control_column_channel:
        return mapping.channel;
    case midi_control_column_data1:
        return mapping.data1;
    case midi_control_column_mode:
        return mapping.interactionMode;
    case midi_control_column_action:
        return mapping.actionType;
    case midi_control_column_target:
        if (mapping.actionType == midi_control_action_type_none)
            return {};
        return mapping.targetName.isEmpty() ? toQString(mapping.targetUuid) : toQString(mapping.targetName);
    case midi_control_column_layer:
        if (mapping.actionType == midi_control_action_type_none)
            return {};
        return mapping.targetLayerName.isEmpty() ? toQString(mapping.targetLayerToken)
                                                 : toQString(mapping.targetLayerName);
    case midi_control_column_parameter:
        if (mapping.actionType == midi_control_action_type_none)
            return {};
        return mapping.targetParameterName.isEmpty() ? toQString(mapping.targetParameterToken)
                                                     : toQString(mapping.targetParameterName);
    case midi_control_column_bank:
        return mapping.bank;
    case midi_control_column_preset:
        return mapping.preset;
    case midi_control_column_activation:
        return mapping.activationMode;
    case midi_control_column_invert:
        return {};
    case midi_control_column_min:
        return mapping.minValue;
    case midi_control_column_max:
        return mapping.maxValue;
    case midi_control_column_scale:
        return mapping.scale;
    default:
        return {};
    }
}

bool MidiControlMappingsTableModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (mappings == nullptr || !index.isValid())
        return false;

    auto row = index.row();
    if (row < 0 || row >= int(mappings->size()))
        return false;

    auto& mapping = (*mappings)[size_t(row)];
    auto column = index.column();

    if (role == Qt::CheckStateRole)
    {
        auto isChecked = value.toInt() == Qt::Checked;

        if (column == midi_control_column_enabled)
            mapping.enabled = isChecked;
        else if (column == midi_control_column_invert)
            mapping.valueInverted = isChecked;
        else
            return false;

        emit dataChanged(index, index, {Qt::CheckStateRole, Qt::DisplayRole, Qt::EditRole});
        return true;
    }

    if (role != Qt::EditRole)
        return false;

    switch (column)
    {
    case midi_control_column_input_device:
    {
        auto text = toJuceString(value.toString()).trim();
        mapping.inputDeviceName = text.equalsIgnoreCase("Any") ? juce::String() : text;
        break;
    }
    case midi_control_column_output_route:
    {
        auto text = toJuceString(value.toString()).trim();
        if (text.equalsIgnoreCase("None") || text.isEmpty())
        {
            mapping.outputMode = midi_control_output_mode_none;
            mapping.outputDeviceName.clear();
        }
        else if (text.equalsIgnoreCase("Any"))
        {
            mapping.outputMode = midi_control_output_mode_any;
            mapping.outputDeviceName.clear();
        }
        else
        {
            mapping.outputMode = midi_control_output_mode_specific;
            mapping.outputDeviceName = text;
        }
        break;
    }
    case midi_control_column_type:
        mapping.messageType = value.toInt();
        break;
    case midi_control_column_channel:
        mapping.channel = value.toInt();
        break;
    case midi_control_column_data1:
        mapping.data1 = value.toInt();
        break;
    case midi_control_column_mode:
        mapping.interactionMode = value.toInt();
        break;
    case midi_control_column_action:
        mapping.actionType = value.toInt();
        break;
    case midi_control_column_target:
    {
        auto text = toJuceString(value.toString()).trim();
        mapping.targetName = text;
        mapping.targetUuid = text;
        break;
    }
    case midi_control_column_layer:
    {
        auto text = toJuceString(value.toString()).trim();
        mapping.targetLayerName = text;
        mapping.targetLayerToken = text;
        break;
    }
    case midi_control_column_parameter:
    {
        auto text = toJuceString(value.toString()).trim();
        mapping.targetParameterName = text;
        mapping.targetParameterToken = text;
        break;
    }
    case midi_control_column_bank:
        mapping.bank = value.toInt();
        break;
    case midi_control_column_preset:
        mapping.preset = value.toInt();
        break;
    case midi_control_column_activation:
        mapping.activationMode = value.toInt();
        break;
    case midi_control_column_min:
        mapping.minValue = value.toDouble();
        break;
    case midi_control_column_max:
        mapping.maxValue = value.toDouble();
        break;
    case midi_control_column_scale:
        mapping.scale = value.toDouble();
        break;
    default:
        return false;
    }

    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    return true;
}

QVariant MidiControlMappingsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return {};

    if (orientation != Qt::Horizontal)
        return {};

    switch (section)
    {
    case midi_control_column_enabled:
        return "On";
    case midi_control_column_learn:
        return "Learn";
    case midi_control_column_input_device:
        return "In Device";
    case midi_control_column_output_route:
        return "Out";
    case midi_control_column_type:
        return "Type";
    case midi_control_column_channel:
        return "Ch";
    case midi_control_column_data1:
        return "Data";
    case midi_control_column_mode:
        return "Mode";
    case midi_control_column_action:
        return "Action";
    case midi_control_column_target:
        return "Target";
    case midi_control_column_layer:
        return "Layer";
    case midi_control_column_parameter:
        return "Parameter";
    case midi_control_column_bank:
        return "Bank";
    case midi_control_column_preset:
        return "Preset";
    case midi_control_column_activation:
        return "Scope";
    case midi_control_column_invert:
        return "Invert";
    case midi_control_column_min:
        return "Min";
    case midi_control_column_max:
        return "Max";
    case midi_control_column_scale:
        return "Scale";
    default:
        return {};
    }
}

Qt::ItemFlags MidiControlMappingsTableModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;

    if (index.column() == midi_control_column_learn)
        return flags;

    flags |= Qt::ItemIsEditable;

    if (index.column() == midi_control_column_enabled || index.column() == midi_control_column_invert)
        flags |= Qt::ItemIsUserCheckable;

    return flags;
}

void MidiControlMappingsTableModel::bindMappings(std::vector<MidiControlMapping>* newMappings)
{
    beginResetModel();
    mappings = newMappings;
    endResetModel();
}

void MidiControlMappingsTableModel::refreshAll()
{
    beginResetModel();
    endResetModel();
}

void MidiControlMappingsTableModel::appendMapping(const MidiControlMapping& mapping)
{
    if (mappings == nullptr)
        return;

    auto row = int(mappings->size());
    beginInsertRows(QModelIndex(), row, row);
    mappings->push_back(mapping);
    endInsertRows();
}

void MidiControlMappingsTableModel::insertMapping(int row, const MidiControlMapping& mapping)
{
    if (mappings == nullptr)
        return;

    auto clampedRow = juce::jlimit(0, int(mappings->size()), row);
    beginInsertRows(QModelIndex(), clampedRow, clampedRow);
    mappings->insert(mappings->begin() + clampedRow, mapping);
    endInsertRows();
}

void MidiControlMappingsTableModel::removeRowsSortedUnique(const std::vector<int>& sortedUniqueRows)
{
    if (mappings == nullptr || sortedUniqueRows.empty())
        return;

    for (int i = int(sortedUniqueRows.size()) - 1; i >= 0; --i)
    {
        auto row = sortedUniqueRows[size_t(i)];
        if (row < 0 || row >= int(mappings->size()))
            continue;

        beginRemoveRows(QModelIndex(), row, row);
        mappings->erase(mappings->begin() + row);
        endRemoveRows();
    }
}

bool MidiControlMappingsTableModel::getMapping(int row, MidiControlMapping& mapping) const
{
    if (mappings == nullptr || row < 0 || row >= int(mappings->size()))
        return false;

    mapping = (*mappings)[size_t(row)];
    return true;
}

bool MidiControlMappingsTableModel::replaceMapping(int row, const MidiControlMapping& mapping)
{
    if (mappings == nullptr || row < 0 || row >= int(mappings->size()))
        return false;

    (*mappings)[size_t(row)] = mapping;
    auto firstColumn = index(row, 0);
    auto lastColumn = index(row, midi_control_column_count - 1);
    emit dataChanged(firstColumn, lastColumn, {Qt::DisplayRole, Qt::EditRole, Qt::CheckStateRole});
    return true;
}

void MidiControlMappingsTableModel::setLearnState(bool active, juce::String mappingId)
{
    if (learnActive == active && learnMappingId == mappingId)
        return;

    learnActive = active;
    learnMappingId = std::move(mappingId);

    if (rowCount() <= 0)
        return;

    auto first = index(0, midi_control_column_learn);
    auto last = index(rowCount() - 1, midi_control_column_learn);
    emit dataChanged(first, last, {Qt::DisplayRole});
}

bool MidiControlMappingsTableModel::isLearningRow(int row) const
{
    if (!learnActive || mappings == nullptr || row < 0 || row >= int(mappings->size()))
        return false;

    return (*mappings)[size_t(row)].mappingId == learnMappingId;
}

} // namespace atk
