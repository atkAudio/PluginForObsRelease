#pragma once

#ifdef ENABLE_QT

#include <atkaudio/midi_obs_mapping.h>

#include <QDialog>

#include <vector>

class QCheckBox;
class QLabel;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QTableWidget;
class QShowEvent;
class QSpinBox;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace atk
{

class MidiToObsDialog : public QDialog
{
public:
    explicit MidiToObsDialog(QWidget* parent = nullptr);
    ~MidiToObsDialog();

protected:
    void showEvent(QShowEvent* event);
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildLayout();
    void reloadFromController();
    void rebuildSubscriptionWidgets();
    void rebuildMappingsTable();
    void applyMappingsToController();
    void syncSubscriptionsToController();
    void startLearningForRow(int row);
    void removeSelectedMappings();
    void addMapping();
    void refreshSourceOptions();
    void pollLearnState();
    bool isValidRow(int row) const;

    QVBoxLayout* mainLayout = nullptr;
    QLabel* statusLabel = nullptr;
    QScrollArea* deviceScrollArea = nullptr;
    QWidget* deviceContainer = nullptr;
    QVBoxLayout* deviceLayout = nullptr;
    QTableWidget* mappingsTable = nullptr;
    QPushButton* addButton = nullptr;
    QPushButton* removeButton = nullptr;
    QPushButton* refreshTargetsButton = nullptr;
    QPushButton* closeButton = nullptr;
    QTimer* learnPollTimer = nullptr;
    std::vector<MidiObsMapping> dialogMappings;
    int lastLearnSequence = 0;
    bool rebuildingUi = false;
};

} // namespace atk

#endif
