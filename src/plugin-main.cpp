/*
This file is part of the atkAudio plugin for OBS.
It is distributed under the AGPLv3 license. See the LICENSE file for details.
*/

#include "CompareVersionStrings.h"
#include "config.h"
#include "core/atkaudio/About.h"
#include "core/atkaudio/GlobalSettings.h"
#include "core/atkaudio/Logging.h"
#include "core/atkaudio/midi_control/midi_control_controller.h"
#include "core/atkaudio/midi_control/midi_control_dialog.h"
#include "core/atkaudio/atkaudio.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <QtWidgets>

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
atk::MidiControlDialog* g_midiToObsDialog = nullptr;
bool g_obsFrontendExiting = false;
bool g_updateCheckInitialized = false;
bool g_loadConflictAlertShown = false;
constexpr bool kForceDuplicateInstallConflictForTesting = false;

obs_module_t* findLoadedModuleConflictByName()
{
    if (kForceDuplicateInstallConflictForTesting)
        return obs_current_module();

    auto* currentModule = obs_current_module();
    auto* loadedModuleByName = obs_get_module(plugin_name);

    if (loadedModuleByName == nullptr || loadedModuleByName == currentModule)
        return nullptr;

    return loadedModuleByName;
}

void showDuplicateInstallAlert(obs_module_t* loadedModule)
{
    if (g_loadConflictAlertShown)
        return;

    g_loadConflictAlertShown = true;

    auto* parent = static_cast<QWidget*>(obs_frontend_get_main_window());

    const char* loadedBinaryPath = loadedModule ? obs_get_module_binary_path(loadedModule) : nullptr;
    const char* loadedFileName = loadedModule ? obs_get_module_file_name(loadedModule) : nullptr;

    const QString message = QString::fromUtf8(
                                "Another PluginForObs installation is already loaded in this OBS session.\n\n"
                                "Plugin name: %1\n"
                                "Attempted version: %2\n"
                                "Loaded module file: %3\n"
                                "Loaded module path: %4\n"
                                "Attempted module path: %5\n\n"
                                "OBS will skip loading this plugin to avoid duplicate registration.\n"
                                "Please keep only one PluginForObs installation/version and restart OBS."
    )
                                .arg(QString::fromUtf8(plugin_name))
                                .arg(PLUGIN_VERSION)
                                .arg(QString::fromUtf8(loadedFileName ? loadedFileName : "(unknown)"))
                                .arg(QString::fromUtf8(loadedBinaryPath ? loadedBinaryPath : "(unknown)"))
                                .arg(QString::fromUtf8(obs_get_module_binary_path(obs_current_module())));

    QMessageBox::critical(parent, "atkAudio Plugin Load Error", message, QMessageBox::Ok);
}

void onFrontendEvent(enum obs_frontend_event event, void* private_data)
{
    UNUSED_PARAMETER(private_data);

    if (event == OBS_FRONTEND_EVENT_THEME_CHANGED)
    {
        atk::getQtMainWindowHandle();

        return;
    }

    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
    {
        atk::getQtMainWindowHandle();

        if (!g_updateCheckInitialized)
        {
            g_updateCheckInitialized = true;
            atk::update();
        }
        return;
    }

    if (event != OBS_FRONTEND_EVENT_EXIT && event != OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
        return;

    g_obsFrontendExiting = true;

    if (g_midiToObsDialog != nullptr)
    {
        g_midiToObsDialog->close();
        g_midiToObsDialog = nullptr;
    }
}

void openGlobalSettingsDialog(void* private_data)
{
    UNUSED_PARAMETER(private_data);

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

    QLabel aboutText(QString::fromUtf8(atk::about::getAboutRichText().c_str()));
    aboutText.setTextFormat(Qt::RichText);
    aboutText.setOpenExternalLinks(true);
    aboutText.setTextInteractionFlags(Qt::TextBrowserInteraction);
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
}

void openMidiControlDialog(void* private_data)
{
    UNUSED_PARAMETER(private_data);

    auto* parent = static_cast<QWidget*>(obs_frontend_get_main_window());

    if (g_midiToObsDialog == nullptr)
    {
        g_midiToObsDialog = new atk::MidiControlDialog(parent);
        g_midiToObsDialog->setAttribute(Qt::WA_DeleteOnClose, true);
        QObject::connect(g_midiToObsDialog, &QObject::destroyed, [](QObject*) { g_midiToObsDialog = nullptr; });
    }

    g_midiToObsDialog->show();
    g_midiToObsDialog->raise();
    g_midiToObsDialog->activateWindow();
}
} // namespace

bool obs_module_load(void)
{
    atk::settings::initialize();
    atk::logging::info("OBS_API", "obs_module_load called");

    auto* loadedModuleConflict = findLoadedModuleConflictByName();
    if (loadedModuleConflict != nullptr)
    {
        const char* loadedBinaryPath = obs_get_module_binary_path(loadedModuleConflict);
        const char* loadedFileName = obs_get_module_file_name(loadedModuleConflict);

        obs_log(
            LOG_ERROR,
            "Detected existing loaded module for plugin name '%s' (file='%s', path='%s'). "
            "Refusing to load PluginForObs version %s to prevent duplicate installations.",
            plugin_name,
            loadedFileName ? loadedFileName : "(unknown)",
            loadedBinaryPath ? loadedBinaryPath : "(unknown)",
            PLUGIN_VERSION
        );

        showDuplicateInstallAlert(loadedModuleConflict);

        atk::logging::error("OBS_API", "obs_module_load failed due duplicate atkAudio installation detection");
        atk::settings::shutdown();
        return false;
    }

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

    g_obsFrontendExiting = false;
    g_updateCheckInitialized = false;
    obs_frontend_add_event_callback(onFrontendEvent, nullptr);

    if (auto* midiObsController = atk::MidiControlController::getInstance())
        midiObsController->initialize();

    // OBS frontend API does not expose extending File->Settings tabs directly.
    // Tools menu item is the supported plugin-level global settings entry point.
    obs_frontend_add_tools_menu_item("atkAudio Plugin", openGlobalSettingsDialog, nullptr);
    obs_frontend_add_tools_menu_item("atkAudio MIDI Control", openMidiControlDialog, nullptr);
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

    obs_frontend_remove_event_callback(onFrontendEvent, nullptr);

    if (!g_obsFrontendExiting && g_midiToObsDialog != nullptr)
    {
        g_midiToObsDialog->close();
        g_midiToObsDialog = nullptr;
    }

    if (auto* midiObsController = atk::MidiControlController::getInstanceWithoutCreating())
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
