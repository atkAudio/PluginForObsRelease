/*
This file is part of the atkAudio plugin for OBS.
It is distributed under the AGPLv3 license. See the LICENSE file for details.
*/

#include "CompareVersionStrings.h"
#include "config.h"
#include "core/atkaudio/About.h"
#include "core/atkaudio/GlobalSettings.h"
#include "core/atkaudio/Logging.h"
#include "core/atkaudio/midi_obs_controller.h"
#include "core/atkaudio/midi_to_obs_dialog.h"
#include "core/atkaudio/atkaudio.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#ifdef ENABLE_QT
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR(PLUGIN_AUTHOR)

MODULE_EXPORT const char* obs_module_name(void)
{
    return PLUGIN_DISPLAY_NAME;
}

const char* plugin_version = PLUGIN_VERSION;
const char* plugin_name = PLUGIN_NAME;

extern struct obs_source_info delay_filter;
extern struct obs_source_info device_io_filter;
extern struct obs_source_info device_io2_filter;
extern struct obs_source_info pluginhost_filter;
extern struct obs_source_info pluginhost2_filter;
extern struct obs_source_info source_mixer;
extern struct obs_source_info ph2helper_source_info;

void obs_log(int log_level, const char* format, ...);

namespace
{
#ifdef ENABLE_QT
atk::MidiToObsDialog* g_midiToObsDialog = nullptr;
bool g_obsFrontendExiting = false;

void onFrontendShutdownEvent(enum obs_frontend_event event, void* private_data)
{
    UNUSED_PARAMETER(private_data);

    if (event != OBS_FRONTEND_EVENT_EXIT && event != OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
        return;

    g_obsFrontendExiting = true;

    if (g_midiToObsDialog != nullptr)
    {
        g_midiToObsDialog->close();
        g_midiToObsDialog = nullptr;
    }
}
#endif

void openGlobalSettingsDialog(void* private_data)
{
    UNUSED_PARAMETER(private_data);

#ifdef ENABLE_QT
    auto* parent = static_cast<QWidget*>(obs_frontend_get_main_window());

    QDialog dialog(parent);
    dialog.setWindowTitle("atkAudio");

    QVBoxLayout layout(&dialog);

    QLabel title("atkAudio");
    title.setStyleSheet("font-weight: bold;");
    layout.addWidget(&title);

    QLabel settingsHeading("Settings");
    settingsHeading.setStyleSheet("font-weight: bold;");
    layout.addWidget(&settingsHeading);

    QCheckBox enableLoggingCheckBox("Enable logging");
    enableLoggingCheckBox.setChecked(atk::settings::isLoggingEnabled());
    layout.addWidget(&enableLoggingCheckBox);

    auto* divider = new QFrame(&dialog);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    layout.addWidget(divider);

    QLabel aboutHeading("About");
    aboutHeading.setStyleSheet("font-weight: bold;");
    layout.addWidget(&aboutHeading);

    QLabel aboutText(QString::fromUtf8(atk::about::getAboutText().c_str()));
    aboutText.setTextFormat(Qt::PlainText);
    aboutText.setTextInteractionFlags(Qt::TextSelectableByMouse);
    aboutText.setWordWrap(true);
    layout.addWidget(&aboutText);

    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout.addWidget(&buttons);

    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted)
    {
        const bool loggingEnabled = enableLoggingCheckBox.isChecked();
        atk::settings::setLoggingEnabled(loggingEnabled);
        blog(LOG_INFO, "[atkAudio][SETTINGS] logging %s", loggingEnabled ? "enabled" : "disabled");
    }
#else
    blog(LOG_WARNING, "[atkAudio][SETTINGS] Qt not available, settings dialog disabled");
#endif
}

void openMidiToObsDialog(void* private_data)
{
    UNUSED_PARAMETER(private_data);

#ifdef ENABLE_QT
    auto* parent = static_cast<QWidget*>(obs_frontend_get_main_window());

    if (g_midiToObsDialog == nullptr)
    {
        g_midiToObsDialog = new atk::MidiToObsDialog(parent);
        g_midiToObsDialog->setAttribute(Qt::WA_DeleteOnClose, true);
        QObject::connect(g_midiToObsDialog, &QObject::destroyed, [](QObject*) { g_midiToObsDialog = nullptr; });
    }

    g_midiToObsDialog->show();
    g_midiToObsDialog->raise();
    g_midiToObsDialog->activateWindow();
#else
    blog(LOG_WARNING, "[atkAudio][MIDI-OBS] Qt not available, MIDI to OBS dialog disabled");
#endif
}
} // namespace

bool obs_module_load(void)
{
    atk::settings::initialize();
    atk::logging::info("OBS_API", "obs_module_load called");

    std::string obsCurrentVersion = obs_get_version_string();
    std::string requiredVersion = PLUGIN_OBS_VERSION_REQUIRED;

    if (CompareVersionStrings(obsCurrentVersion, requiredVersion) < 0)
    {
        obs_log(
            LOG_ERROR,
            "Incompatible OBS version: %s (required: %s)",
            obsCurrentVersion.c_str(),
            requiredVersion.c_str()
        );
        atk::logging::error("OBS_API", "obs_module_load failed due incompatible OBS version");
        atk::settings::shutdown();
        return false;
    }

    // Match obs-plugintemplate default lifecycle log wording.
    obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

    if (!atk::create())
    {
        obs_log(LOG_ERROR, "Failed to initialize OBS JUCE plugin format lifecycle");
        atk::logging::error("OBS_API", "obs_module_load failed while initializing lifecycle");
        atk::settings::shutdown();
        return false;
    }

    auto* mainWindow = (QObject*)obs_frontend_get_main_window();
    if (!atk::startMessagePump(mainWindow))
    {
        obs_log(LOG_ERROR, "Failed to start OBS JUCE plugin format message pump");
        atk::settings::shutdown();
        atk::destroy();
        atk::logging::error("OBS_API", "obs_module_load failed while starting message pump");
        return false;
    }

    atk::update();

#ifdef ENABLE_QT
    g_obsFrontendExiting = false;
    obs_frontend_add_event_callback(onFrontendShutdownEvent, nullptr);
#endif

    if (auto* midiObsController = atk::MidiObsController::getInstance())
        midiObsController->initialize();

    // OBS frontend API does not expose extending File->Settings tabs directly.
    // Tools menu item is the supported plugin-level global settings entry point.
    obs_frontend_add_tools_menu_item("atkAudio Plugin", openGlobalSettingsDialog, nullptr);
    obs_frontend_add_tools_menu_item("atkAudio MIDI to OBS", openMidiToObsDialog, nullptr);
    atk::logging::info("OBS_API", "Registered tools menu item for global atkAudio settings");

    obs_register_source(&delay_filter);
    obs_register_source(&device_io_filter);
    obs_register_source(&device_io2_filter);
    obs_register_source(&pluginhost2_filter);
    obs_register_source(&pluginhost_filter);
    obs_register_source(&source_mixer);
    obs_register_source(&ph2helper_source_info);

    atk::logging::info("OBS_API", "obs_module_load completed");

    return true;
}

void obs_module_unload(void)
{
    atk::logging::info("OBS_API", "obs_module_unload called");

#ifdef ENABLE_QT
    obs_frontend_remove_event_callback(onFrontendShutdownEvent, nullptr);

    if (!g_obsFrontendExiting && g_midiToObsDialog != nullptr)
    {
        g_midiToObsDialog->close();
        g_midiToObsDialog = nullptr;
    }
#endif

    if (auto* midiObsController = atk::MidiObsController::getInstanceWithoutCreating())
    {
        midiObsController->shutdown();
        delete midiObsController;
    }

    // PropertiesFile owns a JUCE timer; release it before JUCE runtime teardown.
    atk::settings::shutdown();

    atk::destroy();

    // Match obs-plugintemplate default lifecycle log wording.
    obs_log(LOG_INFO, "plugin unloaded");
}

void obs_log(int log_level, const char* format, ...)
{
    if (!atk::settings::isLoggingEnabled())
        return;

    size_t length = 4 + strlen(plugin_name) + strlen(format);

    char* templ = (char*)malloc(length + 1);
    snprintf(templ, length, "[%s] %s", plugin_name, format);

    va_list args;
    va_start(args, format);
    blogva(log_level, templ, args);
    va_end(args);

    free(templ);
}
