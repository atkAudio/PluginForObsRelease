#pragma once

#include <atkaudio/midi_control/midi_control_mapping.h>

#include <QtCore>

#include <vector>

namespace atk
{

enum MidiControlTableColumn
{
    midi_control_column_enabled = 0,
    midi_control_column_learn,
    midi_control_column_label,
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
    midi_control_column_min,
    midi_control_column_max,
    midi_control_column_scale,
    midi_control_column_invert,
    midi_control_column_count,
};

class MidiControlMappingsTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    typedef bool (*EditHandler)(void*, const juce::String&, const MidiControlMapping&, juce::String&);

    explicit MidiControlMappingsTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    const std::vector<MidiControlMapping>& getMappings() const;
    void setMappings(const std::vector<MidiControlMapping>& mappings);
    void setEditHandler(EditHandler handler, void* context);
    bool requestMappingEdit(const juce::String& mappingId, const MidiControlMapping& mapping);
    void refreshAll();
    bool getMapping(int row, MidiControlMapping& mapping) const;
    bool replaceMapping(int row, const MidiControlMapping& mapping);
    void setLearnState(bool active, juce::String mappingId);
    bool isLearningRow(int row) const;

signals:
    void editRejected(QString message);

private:
    std::vector<MidiControlMapping> mappings;
    EditHandler editHandler = nullptr;
    void* editContext = nullptr;
    bool learnActive = false;
    juce::String learnMappingId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiControlMappingsTableModel)
};

} // namespace atk
