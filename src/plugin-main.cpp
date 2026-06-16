/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "CompareVersionStrings.h"
#include "config.h"
#include "core/atkaudio/GlobalSettings.h"
#include "core/atkaudio/Logging.h"
#include "core/atkaudio/atkaudio.h"

#include <chrono>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>

#ifdef ENABLE_QT
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

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
void openGlobalSettingsDialog(void* private_data)
{
    UNUSED_PARAMETER(private_data);

#ifdef ENABLE_QT
    auto* parent = static_cast<QWidget*>(obs_frontend_get_main_window());

    QDialog dialog(parent);
    dialog.setWindowTitle("atkAudio Settings");

    QVBoxLayout layout(&dialog);

    QLabel title("Global settings for atkAudio Plugin for OBS");
    title.setStyleSheet("font-weight: bold;");
    layout.addWidget(&title);

    QCheckBox enableLoggingCheckBox("Enable logging");
    enableLoggingCheckBox.setChecked(atk::settings::isLoggingEnabled());
    layout.addWidget(&enableLoggingCheckBox);

    // QLabel note(
    //     "Enables scoped lifecycle/API constructor/destructor logs. "
    //     "Errors are also gated by this setting."
    // );
    // note.setWordWrap(true);
    // layout.addWidget(&note);

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

    // OBS frontend API does not expose extending File->Settings tabs directly.
    // Tools menu item is the supported plugin-level global settings entry point.
    obs_frontend_add_tools_menu_item("atkAudio Settings...", openGlobalSettingsDialog, nullptr);
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
