#pragma once

#include "Editor/PluginWindow.h"
#include "../AudioProcessorGraphMT/AudioProcessorGraphMT.h"
#include <string>

struct NativeEditorHost
{
    virtual ~NativeEditorHost() = default;
    virtual PluginWindow* getOrCreatePluginWindowFor(atk::AudioProcessorGraphMT::Node*, PluginWindow::Type) = 0;
    virtual bool closeAnyOpenPluginWindows() = 0;
    virtual void pruneStalePluginWindows(const atk::AudioProcessorGraphMT&) = 0;
    virtual const std::string& getParentSourceUuid() const = 0;
    virtual const std::string& getOwnerFilterName() const = 0;
    virtual void bindPluginToOwner(juce::AudioPluginInstance*) = 0;
    virtual juce::ApplicationCommandManager& getCommandManager() = 0;
    virtual juce::AudioDeviceManager& getDeviceManager() = 0;
    virtual void addPluginsToMenu(juce::PopupMenu&) = 0;
    virtual void createPluginFromMenu(int, juce::Point<double>) = 0;
};
