#pragma once

namespace atk::settings
{
void initialize();
void shutdown();

bool isLoggingEnabled();
void setLoggingEnabled(bool enabled);
} // namespace atk::settings
