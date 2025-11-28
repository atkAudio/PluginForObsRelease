/*
 * QtParentedWindowDebug.h
 *
 * Platform-specific debug utilities for verifying window parenting.
 */

#pragma once

#include <juce_core/juce_core.h>

namespace atk
{

/**
 * Log detailed window parenting information using platform-specific APIs.
 * On Windows: logs WS_CHILD, GetParent(), GetWindow(GW_OWNER), style flags
 * On macOS: logs NSView hierarchy info
 * On Linux: logs X11 window info
 *
 * @param nativeHandle The native window handle (HWND on Windows, NSView* on macOS)
 * @param expectedParent The parent handle we expected to use
 */
void logWindowParentingInfo(void* nativeHandle, void* expectedParent);

} // namespace atk
