#include "midi_control_active_context_dock.h"

#include <atkaudio/GlobalSettings.h>
#include <atkaudio/midi_control/midi_control_controller.h>

#include <obs-frontend-api.h>

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

namespace
{
constexpr auto activeContextDockId = "atkaudio_midiobs_active_context_dock";
constexpr auto activeContextDockTitle = "MIDI Control Active Bank/Preset";

atk::MidiControlActiveContextDock*& getActiveContextDockInstance()
{
    static atk::MidiControlActiveContextDock* instance = nullptr;
    return instance;
}

QString getActiveContextDisplayText(atk::MidiControlController* controller)
{
    if (controller == nullptr)
        return "Active Bank/Preset: unavailable";

    return QString::fromUtf8("Active Bank/Preset: ")
         + QString::number(controller->getActiveBank())
         + "/"
         + QString::number(controller->getActivePreset());
}

void raiseDockWindow(atk::MidiControlActiveContextDock* dockWidget)
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
} // namespace

namespace atk
{

MidiControlActiveContextDock::MidiControlActiveContextDock(QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(activeContextDockTitle);
    resize(330, 90);

    buildLayout();

    refreshNow();
}

MidiControlActiveContextDock::~MidiControlActiveContextDock() = default;

void MidiControlActiveContextDock::buildLayout()
{
    auto* layout = new QVBoxLayout(this);

    auto* activeTextRow = new QHBoxLayout();
    activeContextLabel = new QLabel("Active Bank/Preset: unavailable", this);
    activeTextRow->addWidget(activeContextLabel);
    activeTextRow->addStretch(1);

    auto* controlsRow = new QHBoxLayout();
    auto* bankLabel = new QLabel("Bank", this);
    activeBankSpin = new QSpinBox(this);
    activeBankSpin->setRange(atk::midi_control_mapping_context_min, atk::midi_control_mapping_context_max);
    auto* presetLabel = new QLabel("Preset", this);
    activePresetSpin = new QSpinBox(this);
    activePresetSpin->setRange(atk::midi_control_mapping_context_min, atk::midi_control_mapping_context_max);

    controlsRow->addWidget(bankLabel);
    controlsRow->addWidget(activeBankSpin);
    controlsRow->addWidget(presetLabel);
    controlsRow->addWidget(activePresetSpin);
    controlsRow->addStretch(1);

    layout->addLayout(activeTextRow);
    layout->addLayout(controlsRow);

    connect(
        activeBankSpin,
        &QSpinBox::valueChanged,
        this,
        [this](int value)
        {
            if (syncingUi)
                return;

            auto* controller = MidiControlController::getInstanceWithoutCreating();
            if (controller == nullptr)
                return;

            controller->setActiveBank(value);
            refreshNow();
        }
    );

    connect(
        activePresetSpin,
        &QSpinBox::valueChanged,
        this,
        [this](int value)
        {
            if (syncingUi)
                return;

            auto* controller = MidiControlController::getInstanceWithoutCreating();
            if (controller == nullptr)
                return;

            controller->setActivePreset(value);
            refreshNow();
        }
    );
}

void MidiControlActiveContextDock::refreshNow()
{
    if (activeContextLabel == nullptr)
        return;

    auto* controller = MidiControlController::getInstanceWithoutCreating();
    activeContextLabel->setText(getActiveContextDisplayText(controller));

    if (activeBankSpin == nullptr || activePresetSpin == nullptr)
        return;

    syncingUi = true;
    QSignalBlocker bankBlocker(activeBankSpin);
    QSignalBlocker presetBlocker(activePresetSpin);

    if (controller == nullptr)
    {
        activeBankSpin->setValue(atk::midi_control_mapping_context_min);
        activePresetSpin->setValue(atk::midi_control_mapping_context_min);
        activeBankSpin->setEnabled(false);
        activePresetSpin->setEnabled(false);
    }
    else
    {
        activeBankSpin->setEnabled(true);
        activePresetSpin->setEnabled(true);
        activeBankSpin->setValue(controller->getActiveBank());
        activePresetSpin->setValue(controller->getActivePreset());
    }

    syncingUi = false;
}

void MidiControlActiveContextDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateRestoreStateSetting();
    refreshNow();
}

void MidiControlActiveContextDock::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    updateRestoreStateSetting();
}

void MidiControlActiveContextDock::updateRestoreStateSetting() const
{
    settings::setRestoreMidiControlActiveContextDock(isVisible() || parentWidget() != nullptr);
}

bool MidiControlActiveContextDock::shouldRestoreOnStartup()
{
    return settings::shouldRestoreMidiControlActiveContextDock();
}

void MidiControlActiveContextDock::ensureRegistered()
{
    auto*& instance = getActiveContextDockInstance();

    if (instance != nullptr)
        return;

    instance = new MidiControlActiveContextDock();
    instance->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(instance, &QObject::destroyed, instance, [](QObject*) { getActiveContextDockInstance() = nullptr; });

    obs_frontend_add_dock_by_id(activeContextDockId, activeContextDockTitle, instance);
}

void MidiControlActiveContextDock::unregisterDock()
{
    auto*& instance = getActiveContextDockInstance();
    if (instance == nullptr)
        return;

    auto* application = QCoreApplication::instance();
    if (application == nullptr)
        return;

    QMetaObject::invokeMethod(
        application,
        []() { obs_frontend_remove_dock(activeContextDockId); },
        Qt::QueuedConnection
    );
}

void MidiControlActiveContextDock::showOrCreateAndRaise()
{
    ensureRegistered();

    auto* instance = getActiveContextDockInstance();
    if (instance == nullptr)
        return;

    instance->refreshNow();
    QTimer::singleShot(0, instance, [instance]() { raiseDockWindow(instance); });
}

void MidiControlActiveContextDock::refreshIfOpen()
{
    auto* instance = getActiveContextDockInstance();
    if (instance != nullptr)
        instance->refreshNow();
}

} // namespace atk
