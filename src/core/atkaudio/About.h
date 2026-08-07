#pragma once

#include <config.h>

#include <string>

namespace atk::about
{
static inline std::string getAboutText()
{
    std::string aboutText = PLUGIN_DISPLAY_NAME;
    aboutText += "\n";
    aboutText += PLUGIN_VERSION;
    aboutText += "\n\nCopyright (c) ";
    aboutText += PLUGIN_YEAR;
    aboutText += " ";
    aboutText += PLUGIN_AUTHOR;
    aboutText += "\nLicensed under AGPLv3";
    return aboutText;
}
} // namespace atk::about
