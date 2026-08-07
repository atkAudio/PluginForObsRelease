#pragma once

#include <atkaudio/midi_control/midi_control_mapping.h>

#include <QtCore>

#include <vector>

namespace atk
{

class MidiControlMappingsTableModel : public QAbstractTableModel
{
public:
    static int getColumnCount();

    explicit MidiControlMappingsTableModel(std::vector<MidiControlMapping>* mappings, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void bindMappings(std::vector<MidiControlMapping>* mappings);
    void refreshAll();
    void appendMapping(const MidiControlMapping& mapping);
    void insertMapping(int row, const MidiControlMapping& mapping);
    void removeRowsSortedUnique(const std::vector<int>& sortedUniqueRows);
    bool getMapping(int row, MidiControlMapping& mapping) const;
    bool replaceMapping(int row, const MidiControlMapping& mapping);
    void setLearnState(bool active, juce::String mappingId);
    bool isLearningRow(int row) const;

private:
    std::vector<MidiControlMapping>* mappings = nullptr;
    bool learnActive = false;
    juce::String learnMappingId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiControlMappingsTableModel)
};

} // namespace atk
