#pragma once

#include <config.h>
#include <juce_audio_utils/juce_audio_utils.h>

static inline void showAboutDialog()
{
    DialogWindow::LaunchOptions options;
    auto& lookAndFeel = juce::LookAndFeel::getDefaultLookAndFeel();
    options.dialogTitle = "About";
    String aboutText;
    aboutText
        << PLUGIN_DISPLAY_NAME
        << "\n"
        << PLUGIN_VERSION
        << "\n\n"
        << "Copyright (c) "
        << PLUGIN_YEAR
        << " "
        << PLUGIN_AUTHOR
        << "\n"
        << "Licensed under AGPL3\n"
        << "\n"
        << "ASIO and VST are registered\n"
        << "trademarks of Steinberg GmbH";
    auto* label = new Label({}, aboutText);
    label->setColour(Label::backgroundColourId, lookAndFeel.findColour(ResizableWindow::backgroundColourId));
    label->setColour(Label::textColourId, lookAndFeel.findColour(Label::textColourId));
    label->setJustificationType(Justification::centred);
    label->setSize(250, 160);
    options.content.setOwned(label);
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.escapeKeyTriggersCloseButton = true;
    options.dialogBackgroundColour = lookAndFeel.findColour(ResizableWindow::backgroundColourId);
    options.launchAsync();
}