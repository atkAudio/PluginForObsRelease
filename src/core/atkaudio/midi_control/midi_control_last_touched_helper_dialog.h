#pragma once

#include <juce_core/juce_core.h>

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <array>
#include <vector>

namespace atk
{

class MidiControlLastTouchedHelperDialog : public QWidget
{
public:
    explicit MidiControlLastTouchedHelperDialog(QWidget* parent = nullptr);
    ~MidiControlLastTouchedHelperDialog() override;

    void refreshNow();
    static bool shouldRestoreOnStartup();
    static void ensureRegistered();
    static void unregisterDock();
    static void showOrCreateAndRaise();
    static void refreshIfOpen();

private:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void updateRestoreStateSetting() const;

    enum SaveScope
    {
        save_scope_plugin = 0,
        save_scope_audio,
        save_scope_video,
    };

    struct SaveEffectParmTarget;

    void buildLayout();
    void updateSaveButtonState();
    void loadSaveDefaults();
    void saveCurrentDefaults();
    std::vector<int> getSelectedSlots() const;
    bool resolveEffectParmTargetForSelection(
        int scope,
        int slotIndex,
        SaveEffectParmTarget& target,
        juce::String& error
    ) const;
    void onSaveAsPresetClicked();

    QTableView* historyTable = nullptr;
    QStandardItemModel* historyModel = nullptr;
    QComboBox* saveScopeCombo = nullptr;
    QSpinBox* destinationBankSpin = nullptr;
    QSpinBox* destinationPresetSpin = nullptr;
    std::array<QCheckBox*, 8> slotChecks = {};
    QLabel* saveStatusLabel = nullptr;
    QPushButton* saveAsPresetButton = nullptr;
    QPushButton* holdHistoryButton = nullptr;
    QPushButton* clearHistoryButton = nullptr;
    QPushButton* refreshButton = nullptr;
    uint64_t lastRenderedTouchSequence = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiControlLastTouchedHelperDialog)
};

} // namespace atk
