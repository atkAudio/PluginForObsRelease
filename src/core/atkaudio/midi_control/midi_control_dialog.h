#pragma once

#include <atkaudio/midi_control/midi_control_mapping.h>

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <memory>
#include <vector>

namespace juce
{
class FileChooser;
}

namespace atk
{
struct MidiControlLearnState;
class MidiControlMappingsTableModel;

class MidiControlDialog : public QDialog
{
public:
    explicit MidiControlDialog(QWidget* parent = nullptr);
    ~MidiControlDialog() override;
    static void refreshIfOpen();

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void buildLayout();
    void reloadFromController();
    void rebuildSubscriptionWidgets();
    void rebuildMappingsTable();
    bool applyMappingsToController();
    void syncSubscriptionsToController();
    void removeSelectedMappings();
    void cloneSelectedMapping();
    void addMapping();
    void refreshSourceOptions();
    void saveMappingsToFile();
    void loadMappingsFromFile();
    void refreshActiveContextDisplay();
    void handleActiveContextChanged(int bank, int preset);
    bool cancelLearningIfActive();
    void showLastTouchedHelperDialog();
    void handleLearnStateChanged(const MidiControlLearnState& learnState);
    bool isValidRow(int row) const;

    QVBoxLayout* mainLayout = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* activeContextLabel = nullptr;
    QPushButton* openActiveContextDockButton = nullptr;
    QCheckBox* parameterAutoSyncCheckBox = nullptr;
    QCheckBox* delayedFeedbackOutputCheckBox = nullptr;
    QCheckBox* matchByNameCheckBox = nullptr;
    QCheckBox* lastTouchedTrackingCheckBox = nullptr;
    QSpinBox* delayedFeedbackOutputIdleMsSpinBox = nullptr;
    QScrollArea* inputDeviceScrollArea = nullptr;
    QWidget* inputDeviceContainer = nullptr;
    QVBoxLayout* inputDeviceLayout = nullptr;
    QScrollArea* outputDeviceScrollArea = nullptr;
    QWidget* outputDeviceContainer = nullptr;
    QVBoxLayout* outputDeviceLayout = nullptr;
    QTableView* mappingsTable = nullptr;
    MidiControlMappingsTableModel* mappingsModel = nullptr;
    QPushButton* addButton = nullptr;
    QPushButton* cloneButton = nullptr;
    QPushButton* removeButton = nullptr;
    QPushButton* saveMappingsButton = nullptr;
    QPushButton* loadMappingsButton = nullptr;
    QPushButton* showLastTouchedHelperButton = nullptr;
    QPushButton* closeButton = nullptr;
    std::unique_ptr<juce::FileChooser> mappingsFileChooser;
    juce::File lastMappingsFile;
    std::vector<MidiControlMapping> dialogMappings;
    int lastLearnSequence = 0;
    int learnStateListenerId = 0;
    int activeContextListenerId = 0;
    int persistentEditorsRow = -1;
    bool rebuildingUi = false;
    bool shuttingDown = false;
};

} // namespace atk
