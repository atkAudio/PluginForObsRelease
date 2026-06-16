#pragma once

#include <juce_core/juce_core.h>

namespace atk::logging
{
enum class Level
{
    debug,
    info,
    warning,
    error
};

void log(Level level, const char* scope, const juce::String& message);

inline void debug(const char* scope, const juce::String& message)
{
    log(Level::debug, scope, message);
}

inline void info(const char* scope, const juce::String& message)
{
    log(Level::info, scope, message);
}

inline void warning(const char* scope, const juce::String& message)
{
    log(Level::warning, scope, message);
}

inline void error(const char* scope, const juce::String& message)
{
    log(Level::error, scope, message);
}
} // namespace atk::logging
