/*
 * QtParentedWindowDebug.cpp
 *
 * Platform-specific debug utilities for verifying window parenting.
 * Separated from header to avoid Windows.h conflicts with JUCE.
 */

#include "QtParentedWindowDebug.h"

#if JUCE_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace atk
{

void logWindowParentingInfo(void* nativeHandle, void* expectedParent)
{
    if (nativeHandle == nullptr)
    {
        DBG("[QtParentedWindow] DEBUG: nativeHandle is null!");
        return;
    }

    auto hwnd = static_cast<HWND>(nativeHandle);
    auto expectedHwnd = static_cast<HWND>(expectedParent);

    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    HWND actualParent = GetParent(hwnd);
    HWND owner = GetWindow(hwnd, GW_OWNER);

    bool isChild = (style & WS_CHILD) != 0;
    bool isPopup = (style & WS_POPUP) != 0;
    bool isOverlapped = (style & WS_OVERLAPPED) != 0;

    DBG("[QtParentedWindow] Win32 Window Analysis:");
    DBG("[QtParentedWindow]   HWND: " + juce::String::toHexString((juce::pointer_sized_int)hwnd));
    DBG("[QtParentedWindow]   Expected parent: " + juce::String::toHexString((juce::pointer_sized_int)expectedHwnd));
    DBG("[QtParentedWindow]   GetParent(): " + juce::String::toHexString((juce::pointer_sized_int)actualParent));
    DBG("[QtParentedWindow]   GetWindow(GW_OWNER): " + juce::String::toHexString((juce::pointer_sized_int)owner));
    DBG("[QtParentedWindow]   Style: " + juce::String::toHexString((unsigned int)style));
    DBG("[QtParentedWindow]   ExStyle: " + juce::String::toHexString((unsigned int)exStyle));
    DBG("[QtParentedWindow]   WS_CHILD: " + juce::String(isChild ? "YES" : "NO"));
    DBG("[QtParentedWindow]   WS_POPUP: " + juce::String(isPopup ? "YES" : "NO"));
    DBG("[QtParentedWindow]   WS_OVERLAPPED: " + juce::String(isOverlapped ? "YES" : "NO"));
    DBG("[QtParentedWindow]   Parent matches expected: " + juce::String(actualParent == expectedHwnd ? "YES" : "NO"));
}

} // namespace atk

#elif JUCE_MAC

namespace atk
{

void logWindowParentingInfo(void* nativeHandle, void* expectedParent)
{
    DBG("[QtParentedWindow] macOS Window Analysis:");
    DBG("[QtParentedWindow]   NSView: " + juce::String::toHexString((juce::pointer_sized_int)nativeHandle));
    DBG("[QtParentedWindow]   Expected parent NSView: "
        + juce::String::toHexString((juce::pointer_sized_int)expectedParent));
    // Could add Objective-C inspection here if needed
}

} // namespace atk

#else // Linux or other

namespace atk
{

void logWindowParentingInfo(void* nativeHandle, void* expectedParent)
{
    DBG("[QtParentedWindow] Linux Window Analysis:");
    DBG("[QtParentedWindow]   X11 Window: " + juce::String::toHexString((juce::pointer_sized_int)nativeHandle));
    DBG("[QtParentedWindow]   Expected parent: " + juce::String::toHexString((juce::pointer_sized_int)expectedParent));
}

} // namespace atk

#endif
