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

static inline std::string getAboutRichText()
{
    std::string aboutText = PLUGIN_DISPLAY_NAME;
    aboutText += "<br>";
    aboutText += PLUGIN_VERSION;
    aboutText += "<br><br>Copyright (c) ";
    aboutText += PLUGIN_YEAR;
    aboutText += " <a href=\"";
    aboutText += PLUGIN_WEBSITE;
    aboutText += "\">";
    aboutText += PLUGIN_AUTHOR;
    aboutText += "</a>";
    aboutText += "<br>Licensed under AGPLv3";
    return aboutText;
}
} // namespace atk::about
