#include "ARAPlugin.h"

#if JUCE_PLUGINHOST_ARA && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)

const Identifier ARAPluginInstanceWrapper::ARATestHost::Context::xmlRootTag{"ARATestHostContext"};
const Identifier ARAPluginInstanceWrapper::ARATestHost::Context::xmlAudioFileAttrib{"AudioFile"};

#endif
