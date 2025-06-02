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

#include "MessagePump.h"
#include "config.h"

#include <atkaudio/atkaudio.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

const char* plugin_version = PLUGIN_VERSION;
const char* plugin_name = PLUGIN_NAME;

extern struct obs_source_info autoreset_filter;
extern struct obs_source_info delay_filter;
extern struct obs_source_info device_io_filter;
extern struct obs_source_info pluginhost_filter;
extern struct obs_source_info source_mixer;

void obs_log(int log_level, const char* format, ...);

MessagePump* messagePump = nullptr;

bool obs_module_load(void)
{
    obs_log(LOG_INFO, "plugin loaded successfully (version %s)", plugin_version);

    auto* mainWindow = (QObject*)obs_frontend_get_main_window();
    messagePump = new MessagePump(mainWindow); // parent handles lifetime

    obs_register_source(&autoreset_filter);
    obs_register_source(&delay_filter);
    obs_register_source(&source_mixer);
    obs_register_source(&device_io_filter);
    obs_register_source(&pluginhost_filter);

    return true;
}

void obs_module_unload(void)
{
    obs_log(LOG_INFO, "plugin unloaded");
}

void obs_log(int log_level, const char* format, ...)
{
    size_t length = 4 + strlen(plugin_name) + strlen(format);

    char* templ = (char*)malloc(length + 1);

    snprintf(templ, length, "[%s] %s", plugin_name, format);

    va_list(args);

    va_start(args, format);
    blogva(log_level, templ, args);
    va_end(args);

    free(templ);
}
