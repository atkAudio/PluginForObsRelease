#pragma once

#include <juce_core/juce_core.h>

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

namespace atk
{

class MidiControlActiveContextDock : public QWidget
{
public:
    explicit MidiControlActiveContextDock(QWidget* parent = nullptr);
    ~MidiControlActiveContextDock() override;

    void refreshNow();

    static bool shouldRestoreOnStartup();
    static void ensureRegistered();
    static void unregisterDock();
    static void showOrCreateAndRaise();
    static void refreshIfOpen();

private:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void buildLayout();
    void updateRestoreStateSetting() const;

    QLabel* activeContextLabel = nullptr;
    QSpinBox* activeBankSpin = nullptr;
    QSpinBox* activePresetSpin = nullptr;
    bool syncingUi = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiControlActiveContextDock)
};

} // namespace atk
